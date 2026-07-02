/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 *
 * l2fwd_dr.c — l2fwd com Dual Receive Ring (TCC DualRing / PPGCC-UFMG)
 *
 * Baseado em dpdk/examples/l2fwd/main.c (DPDK 21.11 / 22.11 LTS).
 * O caminho de TX é idêntico ao l2fwd original (rte_eth_tx_buffer + drain).
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * O QUE MUDOU EM RELAÇÃO À VERSÃO ANTERIOR (MVP1) E POR QUÊ
 * ─────────────────────────────────────────────────────────────────────────────
 * A versão MVP1 NUNCA reduzia perda porque o burst_pool não acrescentava
 * buffer no ponto onde a NIC realmente descarta (a fila de descritores RX).
 * Em DPDK, uma fila RX é ligada a UM único mempool em rte_eth_rx_queue_setup;
 * o burst_pool só servia de destino de cópia para pacotes JÁ recebidos, e a
 * cópia extra só encarecia o caminho — por isso o p99 melhorava (footprint de
 * cache menor) mas a perda não caía (e até subia sob saturação).
 *
 * A correção troca o PAPEL dos dois caminhos. A ideia central do DualRing só
 * reduz perda quando "receber e estacionar" custa MENOS que "processar e
 * encaminhar". Logo, o laço distingue dois regimes pela ocupação da fila RX
 * da NIC (rte_eth_rx_queue_count), que é o sinal direto de descarte iminente:
 *
 *   REGIME AGUDO  (a fila RX da NIC está enchendo → rajada):
 *       APENAS faz spill — copia 64 B para um mbuf em DRAM, libera o mbuf
 *       rápido na hora e volta a fazer poll imediatamente. Sem do_work, sem
 *       MAC, sem TX. Isso MAXIMIZA a taxa com que a fila RX é esvaziada,
 *       evitando o descarte na placa durante a rajada. O trabalho caro é
 *       inteiramente DIFERIDO.
 *   REGIME CALMO  (a fila RX não está enchendo):
 *       drena o trabalho diferido a partir do burst_ring (do_work + MAC + TX),
 *       no ritmo que o núcleo sustenta. Se houver backlog, novas chegadas são
 *       estacionadas atrás dele (preserva ordem FIFO); sem backlog, é o
 *       caminho rápido idêntico ao l2fwd.
 *
 * Por que isto reduz perda (e a versão anterior não reduzia): o gargalo de
 * perda está na fila RX RASA da NIC. Em regime agudo, gastar ciclos no
 * processamento caro afundaria a taxa de esvaziamento da NIC e causaria
 * descarte na placa — por isso o trabalho caro é diferido e a rajada
 * transitória é absorvida no anel PROFUNDO em DRAM (16384 posições), drenado
 * nos vales entre rajadas. Para tráfego cuja MÉDIA cabe na capacidade do
 * núcleo, a fila em DRAM enche na rajada e drena no intervalo → a perda cai
 * de verdade. Sob saturação sustentada (oferta média > capacidade) nenhum
 * buffer resolve: o excedente transborda no burst_pool e é contado como
 * drop_spill (honesto e inevitável), enquanto a vazão converge para a do
 * privRing (degradação graciosa).
 *
 * Correções adicionais:
 *   - do_work() é aplicado EXATAMENTE UMA VEZ por pacote, no caminho que de
 *     fato o encaminha (fast ou drain). Na versão anterior pacotes spillados
 *     pulavam o do_work, viesando a comparação de ciclos/pacote.
 *   - Gatilho de spill baseado na OCUPAÇÃO real da fila RX da NIC
 *     (rte_eth_rx_queue_count) — o sinal direto de "a placa vai descartar" —
 *     com histerese pelo backlog do burst_ring para preservar a ordem FIFO.
 *   - Enquanto houver backlog no burst_ring, novos pacotes continuam sendo
 *     estacionados (não saltam à frente dos já enfileirados) → sem reordenação.
 *
 * Mapeamento conceitual (inspiração AF_XDP, realização 100% DPDK):
 *   fill ring      → fast_pool   (pequeno, LLC-residente, DDIO-friendly)
 *   RX ring        → fila RX da NIC (rte_eth_rx_burst)
 *   TX ring        → fila TX da NIC (rte_eth_tx_buffer + drain)
 *   overflow/DRAM  → burst_pool + burst_ring (absorvem a rajada)
 *
 * Uso (igual l2fwd, mais os parâmetros DR):
 *   sudo ./l2fwd_dr -l 0-1 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 \
 *       -- -p 0x3 -T 2 \
 *       --rx-ring-size 128 --fast-mbufs 2048 \
 *       --burst-mbufs 65536 --rx-burst-watermark 64
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <sys/types.h>
#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>

#include <rte_common.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_memory.h>
#include <rte_memcpy.h>
#include <rte_eal.h>
#include <rte_launch.h>
#include <rte_cycles.h>
#include <rte_prefetch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>
#include <rte_branch_prediction.h>
#include <rte_debug.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_ring.h>

/* ── Constantes l2fwd ──────────────────────────────────────────────────────── */

#define RTE_LOGTYPE_L2FWD        RTE_LOGTYPE_USER1
#define MAX_PKT_BURST            32
#define BURST_TX_DRAIN_US        100     /* drain TX a cada ~100 µs (igual l2fwd) */
#define MEMPOOL_CACHE_SIZE       256
#define MAX_RX_QUEUE_PER_LCORE   16
#define TX_RING_SIZE             1024

/* Verificação de link */
#define CHECK_INTERVAL_MS        100
#define MAX_CHECK_TIME           90      /* 9 s */

/* ── Constantes Dual Ring ──────────────────────────────────────────────────── */

#define DR_BURST_RING_ENTRIES    16384   /* potência-de-2; reside em DRAM        */
#define DR_DRAIN_BATCH           32      /* mbufs drenados (e processados) por vez */

/* ── Configuração CLI ─────────────────────────────────────────────────────── */

/* l2fwd original */
static uint32_t l2fwd_enabled_port_mask = 0;
static bool     mac_updating            = true;
static int      timer_period            = 10;    /* segundos; 0 = off */

/* DR: parâmetros */
static unsigned nb_fast_mbufs       = 2048;   /* fast pool pequeno (LLC)        */
static unsigned nb_burst_mbufs      = 65536;  /* overflow pool (DRAM)           */
static uint16_t rx_ring_size        = 128;    /* descritores RX da NIC          */
static uint16_t rx_burst_watermark  = 0;      /* spill quando ocupação RX >= N  */
                                              /* (0 ⇒ auto = rx_ring_size/2)    */
static unsigned spill_watermark     = 256;    /* guarda de fome de mbufs:       */
                                              /* spill se fast_avail < N        */
static bool     disable_burst       = false;  /* --disable-burst: modo privRing */

/* DR: simulação de workload (réplica do WorkPackage do ShRing/FastClick) */
static uint8_t  *work_buf       = NULL;
static uint64_t  work_buf_sz    = 0;     /* bytes; 0 = desabilitado            */
static unsigned  work_per_pkt   = 0;     /* acessos aleatórios por pacote      */

/* ── Estado global ────────────────────────────────────────────────────────── */

static volatile bool force_quit = false;

static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* Pools compartilhados entre portas. O rx_queue_setup de cada porta registra o
 * bloco de hugepages do fast_pool em todos os PDs do mlx5, tornando seus mbufs
 * válidos para TX cross-port (igual ao l2fwd). O burst_pool não passa por
 * rx_queue_setup; no mlx5 o TX desses mbufs é coberto pela MR cache dinâmica do
 * PMD (registro sob demanda na primeira transmissão de cada chunk). */
static struct rte_mempool *fast_pool  = NULL;
static struct rte_mempool *burst_pool = NULL;

/* burst ring por porta de ingresso. Enqueue (spill) e dequeue (drain) ocorrem
 * no MESMO lcore que processa a porta, portanto SP/SC é correto e mais rápido. */
static struct rte_ring *dr_burst_ring[RTE_MAX_ETHPORTS];

static uint64_t timer_period_tsc = 0;

/* ── Configuração por lcore ───────────────────────────────────────────────── */

struct lcore_queue_conf {
    unsigned n_rx_port;
    unsigned rx_port_list[MAX_RX_QUEUE_PER_LCORE];
    struct rte_eth_dev_tx_buffer *tx_buffer[RTE_MAX_ETHPORTS];
} __rte_cache_aligned;

static struct lcore_queue_conf lcore_queue_conf[RTE_MAX_LCORE];

/* ── Estatísticas ─────────────────────────────────────────────────────────── */

struct l2fwd_port_statistics {
    uint64_t tx;
    uint64_t rx;
    uint64_t dropped;       /* descartes do tx_buffer (NIC TX cheia)           */
    /* DR */
    uint64_t spilled;       /* pacotes estacionados em DRAM (fast → burst_ring) */
    uint64_t drained;       /* pacotes processados a partir do burst_ring       */
    uint64_t drop_spill;    /* descartados no spill (burst_pool/ring cheios)     */
    uint64_t max_backlog;   /* maior ocupação observada no burst_ring           */
} __rte_cache_aligned;

static struct l2fwd_port_statistics port_statistics[RTE_MAX_ETHPORTS];

/* ── Configuração de porta ────────────────────────────────────────────────── */

static struct rte_eth_conf port_conf = {
    .rxmode = { .mq_mode = RTE_ETH_MQ_RX_NONE },
    .txmode = { .mq_mode = RTE_ETH_MQ_TX_NONE },
};

/* ── Sinal ────────────────────────────────────────────────────────────────── */

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\nSinal %d recebido, encerrando...\n", signum);
        force_quit = true;
    }
}

/* ── Estatísticas ─────────────────────────────────────────────────────────── */

static void
print_stats(void)
{
    uint64_t total_rx = 0, total_tx = 0, total_dropped = 0;
    uint64_t total_spilled = 0, total_drained = 0, total_drop_spill = 0;
    uint16_t portid;

    const char clr[]     = { 27, '[', '2', 'J', '\0' };
    const char topleft[] = { 27, '[', '1', ';', '1', 'H', '\0' };
    printf("%s%s", clr, topleft);

    printf("\n══════════════ l2fwd_dr: estatísticas ══════════════\n");

    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
            continue;

        unsigned fast_avail = fast_pool ?
            rte_mempool_avail_count(fast_pool) : 0;
        unsigned backlog = dr_burst_ring[portid] ?
            rte_ring_count(dr_burst_ring[portid]) : 0;

        printf("\n Porta %u ────────────────────────────────────────\n", portid);
        printf("  rx          : %16" PRIu64 "\n", port_statistics[portid].rx);
        printf("  tx          : %16" PRIu64 "\n", port_statistics[portid].tx);
        printf("  dropped(tx) : %16" PRIu64 "\n", port_statistics[portid].dropped);
        printf("  [DR] spilled    : %12" PRIu64 "  (fast → burst_ring)\n",
               port_statistics[portid].spilled);
        printf("  [DR] drained    : %12" PRIu64 "  (burst_ring → processado/TX)\n",
               port_statistics[portid].drained);
        printf("  [DR] drop_spill : %12" PRIu64 "  (overflow real — média > capacidade)\n",
               port_statistics[portid].drop_spill);
        printf("  [DR] backlog    : %12u  (max %" PRIu64 ", de %u)\n",
               backlog, port_statistics[portid].max_backlog, DR_BURST_RING_ENTRIES);
        printf("  [DR] fast_avail : %12u  (de %u mbufs)\n",
               fast_avail, nb_fast_mbufs);

        total_rx         += port_statistics[portid].rx;
        total_tx         += port_statistics[portid].tx;
        total_dropped    += port_statistics[portid].dropped;
        total_spilled    += port_statistics[portid].spilled;
        total_drained    += port_statistics[portid].drained;
        total_drop_spill += port_statistics[portid].drop_spill;
    }

    printf("\n Total ──────────────────────────────────────────────\n");
    printf("  rx          : %16" PRIu64 "\n", total_rx);
    printf("  tx          : %16" PRIu64 "\n", total_tx);
    printf("  dropped(tx) : %16" PRIu64 "\n", total_dropped);
    printf("  [DR] spilled    : %12" PRIu64 "\n", total_spilled);
    printf("  [DR] drained    : %12" PRIu64 "\n", total_drained);
    printf("  [DR] drop_spill : %12" PRIu64 "\n", total_drop_spill);
    printf("═════════════════════════════════════════════════════\n");
}

/* ── Simulação de workload (WorkPackage — réplica do ShRing/FastClick) ────── */

/* Faz 'work_per_pkt' acessos aleatórios a 'work_buf' de tamanho 'work_buf_sz'.
 * Simula uma função de rede (NAT/firewall) que toca dados além do cabeçalho,
 * com footprint de memória configurável. A semente varia por pacote para
 * impedir prefetch especulativo do hardware. */
static inline void
do_work(struct rte_mbuf *pkt)
{
    if (likely(work_per_pkt == 0 || work_buf == NULL))
        return;
    uint32_t seed = rte_be_to_cpu_32(
        *rte_pktmbuf_mtod_offset(pkt, uint32_t *,
                                 offsetof(struct rte_ether_hdr, src_addr)));
    for (unsigned k = 0; k < work_per_pkt; k++) {
        seed = seed * 1664525u + 1013904223u;     /* LCG — distribui uniformemente */
        volatile uint8_t *p = work_buf + (seed % work_buf_sz);
        (void)*p;
    }
}

/* ── MAC update (idêntico l2fwd) ─────────────────────────────────────────── */

static inline void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    /* dst: 02:00:00:00:00:XX (locally-administered; passa pelo filtro DMAC do
     * mlx5 bifurcado sem promiscuous). */
    uint8_t *tmp = &eth->dst_addr.addr_bytes[0];
    *((uint64_t *)tmp) = 0x000000000002ULL + ((uint64_t)dest_portid << 40);

    rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->src_addr);
}

/* ── Processamento completo de um pacote (caminho caro) ──────────────────────
 * do_work + MAC + TX-buffer. Usado TANTO pelo caminho rápido QUANTO pela
 * drenagem do burst_ring, garantindo que cada pacote receba o workload
 * exatamente uma vez, independentemente do caminho. */
static inline void
l2fwd_forward_one(struct rte_mbuf *m, unsigned src_portid,
                  struct lcore_queue_conf *qconf)
{
    unsigned dst_port = l2fwd_dst_ports[src_portid];

    do_work(m);

    if (mac_updating)
        l2fwd_mac_updating(m, dst_port);

    int sent = rte_eth_tx_buffer(dst_port, 0, qconf->tx_buffer[dst_port], m);
    if (sent)
        port_statistics[dst_port].tx += (uint64_t)sent;
}

/* Encaminha um lote inteiro pelo caminho caro (do_work + MAC + TX), com
 * prefetch do próximo pacote. Usado pelo modo privRing e pelo caminho rápido
 * do DualRing. */
static inline void
l2fwd_forward_batch(struct rte_mbuf **pkts, uint16_t nb, unsigned portid,
                    struct lcore_queue_conf *qconf)
{
    for (uint16_t j = 0; j < nb; j++) {
        if (j + 1 < nb)
            rte_prefetch0(rte_pktmbuf_mtod(pkts[j + 1], void *));
        l2fwd_forward_one(pkts[j], portid, qconf);
    }
}

/* ── DR: cópia barata fast → DRAM ─────────────────────────────────────────────
 * Aloca um mbuf no burst_pool e copia apenas os bytes do pacote. Caminho rápido
 * para pacotes de segmento único (caso do testbed, 64 B); cai para a cópia
 * genérica do DPDK apenas se o pacote for segmentado. NÃO faz do_work/MAC — o
 * propósito do spill é tirar o pacote da NIC o mais barato possível. */
static inline struct rte_mbuf *
dr_stash_copy(struct rte_mbuf *src)
{
    struct rte_mbuf *dst = rte_pktmbuf_alloc(burst_pool);
    if (unlikely(dst == NULL))
        return NULL;

    if (unlikely(src->nb_segs > 1)) {
        rte_pktmbuf_free(dst);
        return rte_pktmbuf_copy(src, burst_pool, 0, UINT32_MAX);
    }

    const uint16_t len = rte_pktmbuf_pkt_len(src);
    rte_memcpy(rte_pktmbuf_mtod(dst, void *),
               rte_pktmbuf_mtod(src, void *), len);
    dst->data_len = len;
    dst->pkt_len  = len;
    return dst;
}

/* ── DR: spill — estaciona um lote inteiro em DRAM ───────────────────────────
 * Copia os nb pacotes para o burst_pool, libera TODOS os mbufs rápidos de uma
 * vez (devolvendo-os ao fast_pool para a NIC repor descritores) e enfileira as
 * cópias no burst_ring em bulk. Pacotes que não couberem (burst_pool ou ring
 * cheios) são contados como drop_spill — o único descarte legítimo do DualRing,
 * que só ocorre quando a oferta MÉDIA excede a capacidade de processamento. */
static void
dr_spill_pkts(struct rte_mbuf **pkts, uint16_t nb, unsigned portid)
{
    struct rte_mbuf *copies[MAX_PKT_BURST];
    uint16_t nc = 0;

    for (uint16_t i = 0; i < nb; i++) {
        if (i + 1 < nb)
            rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 1], void *));
        struct rte_mbuf *c = dr_stash_copy(pkts[i]);
        if (likely(c != NULL))
            copies[nc++] = c;
        else
            port_statistics[portid].drop_spill++;
    }

    /* Libera os mbufs rápidos imediatamente → fill ring reposto sem esperar TX. */
    rte_pktmbuf_free_bulk(pkts, nb);

    unsigned enq = rte_ring_enqueue_burst(dr_burst_ring[portid],
                                          (void *const *)copies, nc, NULL);
    if (unlikely(enq < nc)) {
        rte_pktmbuf_free_bulk(&copies[enq], nc - enq);
        port_statistics[portid].drop_spill += (nc - enq);
    }
    port_statistics[portid].spilled += enq;
}

/* ── DR: drena o burst_ring fazendo o processamento caro DIFERIDO ────────────
 * Cada pacote drenado recebe agora do_work + MAC + TX. É aqui que o trabalho
 * adiado durante a rajada é executado, no ritmo que o núcleo sustenta. */
static unsigned
dr_drain_burst_ring(unsigned portid, struct lcore_queue_conf *qconf,
                    unsigned max_drain)
{
    struct rte_mbuf *bp[DR_DRAIN_BATCH];
    if (max_drain > DR_DRAIN_BATCH)
        max_drain = DR_DRAIN_BATCH;

    unsigned n = rte_ring_dequeue_burst(dr_burst_ring[portid],
                                        (void **)bp, max_drain, NULL);
    for (unsigned i = 0; i < n; i++) {
        if (i + 1 < n)
            rte_prefetch0(rte_pktmbuf_mtod(bp[i + 1], void *));
        l2fwd_forward_one(bp[i], portid, qconf);
    }
    port_statistics[portid].drained += n;
    return n;
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

static void
l2fwd_main_loop(void)
{
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    unsigned lcore_id;
    uint64_t prev_tsc = 0, diff_tsc, cur_tsc, timer_tsc = 0;
    unsigned i, portid, nb_rx;
    struct lcore_queue_conf *qconf;

    const uint64_t drain_tsc =
        (rte_get_tsc_hz() + 1000000ULL - 1) / 1000000ULL * BURST_TX_DRAIN_US;

    lcore_id = rte_lcore_id();
    qconf    = &lcore_queue_conf[lcore_id];

    if (qconf->n_rx_port == 0) {
        RTE_LOG(INFO, L2FWD, "lcore %u: sem portas atribuídas\n", lcore_id);
        return;
    }

    RTE_LOG(INFO, L2FWD, "lcore %u: iniciando main loop\n", lcore_id);
    for (i = 0; i < qconf->n_rx_port; i++)
        RTE_LOG(INFO, L2FWD, "  porta RX %u\n", qconf->rx_port_list[i]);

    while (!force_quit) {
        cur_tsc  = rte_rdtsc();
        diff_tsc = cur_tsc - prev_tsc;

        /* ── TX drain + stats periódicas (idêntico l2fwd) ─────────────── */
        if (unlikely(diff_tsc > drain_tsc)) {
            for (i = 0; i < qconf->n_rx_port; i++) {
                portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
                int sent = rte_eth_tx_buffer_flush(portid, 0,
                                                   qconf->tx_buffer[portid]);
                if (sent)
                    port_statistics[portid].tx += (uint64_t)sent;
            }

            if (timer_period_tsc > 0) {
                timer_tsc += diff_tsc;
                if (unlikely(timer_tsc >= timer_period_tsc)) {
                    if (lcore_id == rte_get_main_lcore()) {
                        print_stats();
                        timer_tsc = 0;
                    }
                }
            }
            prev_tsc = cur_tsc;
        }

        /* ── RX + Dual Ring forwarding ────────────────────────────────── */
        for (i = 0; i < qconf->n_rx_port; i++) {
            portid = qconf->rx_port_list[i];
            nb_rx  = rte_eth_rx_burst(portid, 0, pkts_burst, MAX_PKT_BURST);

            /* ── MODO privRing (baseline) ──────────────────────────────────
             * Caminho único: processa tudo inline. É o método clássico contra
             * o qual o DualRing é comparado. */
            if (disable_burst) {
                if (likely(nb_rx > 0)) {
                    port_statistics[portid].rx += nb_rx;
                    l2fwd_forward_batch(pkts_burst, (uint16_t)nb_rx,
                                        portid, qconf);
                }
                continue;
            }

            /* ── MODO DualRing ─────────────────────────────────────────────
             * A decisão central distingue DOIS regimes pela ocupação da fila
             * RX da NIC — o sinal direto de descarte iminente na placa:
             *
             *   AGUDO  (a NIC está enchendo): SÓ faz spill. Estaciona o lote
             *          em DRAM o mais barato possível e volta a fazer poll
             *          imediatamente. Não há processamento caro aqui, então a
             *          fila RX é esvaziada na máxima taxa → a placa não
             *          descarta durante a rajada.
             *   CALMO  (a NIC não está enchendo): drena o trabalho diferido.
             *          Se houver backlog, encaminha-o a partir do burst_ring
             *          (caro); novas chegadas são estacionadas atrás para
             *          preservar a ordem FIFO. Sem backlog, é o caminho rápido
             *          (idêntico ao l2fwd, sem penalidade).
             *
             * É essa separação que reduz a perda: a rajada transitória é
             * absorvida no anel profundo em DRAM (16384) e drenada nos vales
             * entre rajadas, em vez de ser descartada na fila RX rasa da NIC. */
            unsigned backlog = rte_ring_count(dr_burst_ring[portid]);
            if (backlog > port_statistics[portid].max_backlog)
                port_statistics[portid].max_backlog = backlog;

            if (likely(nb_rx > 0))
                port_statistics[portid].rx += nb_rx;

            /* Sinais de rajada: ocupação da fila RX (sinal primário e direto),
             * lote cheio (forte indício de mais pacotes esperando — robusto
             * caso rte_eth_rx_queue_count não seja suportado), ou fome de
             * mbufs no fast_pool. (qc < 0 ⇒ não suportado ⇒ tratado como 0.) */
            int qc = rte_eth_rx_queue_count(portid, 0);
            unsigned qoccup = (qc > 0) ? (unsigned)qc : 0;
            bool acute = (qoccup >= rx_burst_watermark);

            if (acute) {
                /* Rescue: drena a NIC barato. Mantém o working set pequeno
                 * (libera o mbuf rápido na hora) e não toca em trabalho caro. */
                if (nb_rx > 0)
                    dr_spill_pkts(pkts_burst, (uint16_t)nb_rx, portid);
            } else if (backlog > 0) {
                /* NIC calma, mas há trabalho diferido. Preserva FIFO: as novas
                 * chegadas entram atrás do backlog; depois drena uma fatia. */
                if (nb_rx > 0)
                    dr_spill_pkts(pkts_burst, (uint16_t)nb_rx, portid);
                dr_drain_burst_ring(portid, qconf, DR_DRAIN_BATCH);
            } else if (nb_rx > 0) {
                /* Caminho rápido: regime estável, anel vazio. */
                l2fwd_forward_batch(pkts_burst, (uint16_t)nb_rx, portid, qconf);
            }
        }
    }
}

static int
l2fwd_launch_one_lcore(__rte_unused void *dummy)
{
    l2fwd_main_loop();
    return 0;
}

/* ── Verificação de link ──────────────────────────────────────────────────── */

static void
check_all_ports_link_status(uint32_t port_mask)
{
    uint16_t portid;
    uint8_t count, all_up, print_flag = 0;
    struct rte_eth_link link;
    int ret;
    char link_str[RTE_ETH_LINK_MAX_STR_LEN];

    printf("\nChecando status dos links");
    fflush(stdout);

    for (count = 0; count <= MAX_CHECK_TIME; count++) {
        if (force_quit) return;
        all_up = 1;
        RTE_ETH_FOREACH_DEV(portid) {
            if (force_quit) return;
            if ((port_mask & (1u << portid)) == 0)
                continue;
            memset(&link, 0, sizeof(link));
            ret = rte_eth_link_get_nowait(portid, &link);
            if (ret < 0) {
                all_up = 0;
                if (print_flag)
                    printf("Porta %u: falha ao obter link: %s\n",
                           portid, rte_strerror(-ret));
                continue;
            }
            if (print_flag) {
                rte_eth_link_to_str(link_str, sizeof(link_str), &link);
                printf("Porta %u: %s\n", portid, link_str);
                continue;
            }
            if (link.link_status == RTE_ETH_LINK_DOWN)
                all_up = 0;
        }
        if (print_flag) {
            printf("\n");
            break;
        }
        if (!all_up) {
            printf(".");
            fflush(stdout);
            rte_delay_ms(CHECK_INTERVAL_MS);
        }
        if (all_up || count == (MAX_CHECK_TIME - 1))
            print_flag = 1;
    }
}

/* ── Inicialização de porta ───────────────────────────────────────────────── */

static int
l2fwd_port_init(uint16_t portid)
{
    struct rte_eth_conf     local_conf = port_conf;
    struct rte_eth_dev_info dev_info;
    struct rte_eth_txconf   txconf;
    uint16_t nb_rxd = rx_ring_size, nb_txd = TX_RING_SIZE;
    int ret;

    if (!rte_eth_dev_is_valid_port(portid))
        return -1;

    ret = rte_eth_dev_info_get(portid, &dev_info);
    if (ret != 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_info_get porta %u: %s\n",
                 portid, strerror(-ret));

    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        local_conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    ret = rte_eth_dev_configure(portid, 1, 1, &local_conf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_configure porta %u: %s\n",
                 portid, strerror(-ret));

    ret = rte_eth_dev_adjust_nb_rx_tx_desc(portid, &nb_rxd, &nb_txd);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_adjust_nb_rx_tx_desc porta %u: %s\n",
                 portid, strerror(-ret));

    ret = rte_eth_macaddr_get(portid, &l2fwd_ports_eth_addr[portid]);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_macaddr_get porta %u: %s\n",
                 portid, strerror(-ret));

    /* Fila RX alimentada pelo fast_pool (fill ring pequeno, LLC-residente). */
    ret = rte_eth_rx_queue_setup(portid, 0, nb_rxd,
                                 rte_eth_dev_socket_id(portid),
                                 NULL, fast_pool);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup porta %u: %s\n",
                 portid, strerror(-ret));

    txconf          = dev_info.default_txconf;
    txconf.offloads = local_conf.txmode.offloads;
    ret = rte_eth_tx_queue_setup(portid, 0, nb_txd,
                                 rte_eth_dev_socket_id(portid), &txconf);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup porta %u: %s\n",
                 portid, strerror(-ret));

    ret = rte_eth_dev_start(portid);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "rte_eth_dev_start porta %u: %s\n",
                 portid, strerror(-ret));

    ret = rte_eth_promiscuous_enable(portid);
    if (ret != 0 && ret != -ENOTSUP)
        rte_exit(EXIT_FAILURE, "rte_eth_promiscuous_enable porta %u: %s\n",
                 portid, strerror(-ret));

    printf("Porta %u: MAC=" RTE_ETHER_ADDR_PRT_FMT " → dst=porta %u\n",
           portid,
           RTE_ETHER_ADDR_BYTES(&l2fwd_ports_eth_addr[portid]),
           l2fwd_dst_ports[portid]);
    return 0;
}

/* ── CLI ──────────────────────────────────────────────────────────────────── */

static void
usage(const char *prgname)
{
    printf(
        "Uso: %s [EAL] -- [opções]\n"
        "  -p PORTMASK            portas em hex (padrão: todas)\n"
        "  --no-mac-updating      desabilita MAC update\n"
        "  -T SEGUNDOS            intervalo de stats (padrão %d; 0=off)\n"
        "\n"
        "  Dual Ring (DR):\n"
        "  --fast-mbufs N         tamanho do fast pool/fill ring (padrão %u)\n"
        "  --burst-mbufs N        tamanho do burst pool em DRAM (padrão %u)\n"
        "  --rx-ring-size N       descritores RX da NIC (padrão %u; pot-de-2)\n"
        "  --rx-burst-watermark N spill quando ocupação RX >= N descritores\n"
        "                         (padrão: rx-ring-size/2)\n"
        "  --spill-watermark N    guarda de fome: spill se fast_avail < N (padrão %u)\n"
        "  --disable-burst        modo privRing (só fast pool, sem spill)\n"
        "\n"
        "  Workload (WorkPackage, réplica do ShRing):\n"
        "  --work-mem-mb M        buffer de trabalho em MiB (padrão: 0=off)\n"
        "  --work-per-pkt N       acessos aleatórios por pacote (padrão: 0)\n",
        prgname, timer_period, nb_fast_mbufs, nb_burst_mbufs,
        rx_ring_size, spill_watermark);
}

static uint32_t
parse_portmask(const char *portmask)
{
    char *end = NULL;
    unsigned long pm = strtoul(portmask, &end, 16);
    if (portmask[0] == '\0' || end == NULL || *end != '\0' || pm == 0)
        return 0;
    return (uint32_t)pm;
}

static int
l2fwd_parse_args(int argc, char **argv)
{
    static const struct option lgopts[] = {
        { "no-mac-updating",    no_argument,       NULL, 0 },
        { "fast-mbufs",         required_argument, NULL, 0 },
        { "burst-mbufs",        required_argument, NULL, 0 },
        { "rx-ring-size",       required_argument, NULL, 0 },
        { "rx-burst-watermark", required_argument, NULL, 0 },
        { "spill-watermark",    required_argument, NULL, 0 },
        { "disable-burst",      no_argument,       NULL, 0 },
        { "work-mem-mb",        required_argument, NULL, 0 },
        { "work-per-pkt",       required_argument, NULL, 0 },
        { NULL, 0, NULL, 0 },
    };
    int opt, option_index;
    char *prgname = argv[0];

    while ((opt = getopt_long(argc, argv, "p:T:h", lgopts, &option_index)) != EOF) {
        switch (opt) {
        case 'p':
            l2fwd_enabled_port_mask = parse_portmask(optarg);
            if (l2fwd_enabled_port_mask == 0) {
                printf("portmask inválido: %s\n", optarg);
                usage(prgname);
                return -1;
            }
            break;

        case 'T':
            timer_period = atoi(optarg);
            if (timer_period < 0) {
                printf("timer_period inválido\n");
                usage(prgname);
                return -1;
            }
            break;

        case 'h':
            usage(prgname);
            return -1;

        case 0:
            if (!strcmp(lgopts[option_index].name, "no-mac-updating"))
                mac_updating = false;
            else if (!strcmp(lgopts[option_index].name, "fast-mbufs"))
                nb_fast_mbufs = (unsigned)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "burst-mbufs"))
                nb_burst_mbufs = (unsigned)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "rx-ring-size"))
                rx_ring_size = (uint16_t)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "rx-burst-watermark"))
                rx_burst_watermark = (uint16_t)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "spill-watermark"))
                spill_watermark = (unsigned)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "disable-burst"))
                disable_burst = true;
            else if (!strcmp(lgopts[option_index].name, "work-mem-mb"))
                work_buf_sz = (uint64_t)atoi(optarg) * 1024 * 1024;
            else if (!strcmp(lgopts[option_index].name, "work-per-pkt"))
                work_per_pkt = (unsigned)atoi(optarg);
            break;

        default:
            usage(prgname);
            return -1;
        }
    }

    if (optind >= 0)
        argv[optind - 1] = prgname;
    int ret = optind - 1;
    optind = 1;
    return ret;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    struct lcore_queue_conf *qconf = NULL;
    uint16_t portid, last_port = 0;
    unsigned lcore_id, rx_lcore_id;
    unsigned nb_ports_in_mask = 0, nb_ports_available = 0, nb_lcores = 0;
    int ret;

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Erro na inicialização EAL\n");
    argc -= ret;
    argv += ret;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    ret = l2fwd_parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Argumentos inválidos\n");

    /* rx_burst_watermark padrão = metade do ring RX (ocupação ≥ 50% ⇒ rajada). */
    if (rx_burst_watermark == 0)
        rx_burst_watermark = (rx_ring_size > 2) ? (rx_ring_size / 2) : 1;

    printf("MAC updating      : %s\n", mac_updating ? "sim" : "não");
    printf("Stats timer       : %d s\n", timer_period);
    printf("fast-mbufs        : %u\n", nb_fast_mbufs);
    printf("burst-mbufs       : %u%s\n", nb_burst_mbufs,
           disable_burst ? " (DESABILITADO — privRing)" : "");
    printf("rx-ring-size      : %u\n", rx_ring_size);
    printf("rx-burst-watermark: %u\n", rx_burst_watermark);
    printf("spill-watermark   : %u\n", spill_watermark);
    printf("disable-burst     : %s\n", disable_burst ? "sim" : "não");
    printf("work-mem-mb       : %u MiB\n", (unsigned)(work_buf_sz >> 20));
    printf("work-per-pkt      : %u\n", work_per_pkt);

    if (timer_period > 0)
        timer_period_tsc = (uint64_t)timer_period * rte_get_tsc_hz();

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "Nenhuma porta Ethernet disponível\n");

    if (l2fwd_enabled_port_mask == 0) {
        RTE_ETH_FOREACH_DEV(portid)
            l2fwd_enabled_port_mask |= (1u << portid);
    }

    /* Mapeamento de destino: portid → dst_portid (par: 0↔1, 2↔3, ...) */
    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
            continue;
        if (nb_ports_in_mask % 2) {
            l2fwd_dst_ports[portid]    = last_port;
            l2fwd_dst_ports[last_port] = portid;
        } else {
            last_port = portid;
        }
        nb_ports_in_mask++;
    }
    if (nb_ports_in_mask % 2) {
        printf("AVISO: número ímpar de portas — última porta ecoa em si mesma\n");
        l2fwd_dst_ports[last_port] = last_port;
    }

    /* Distribui portas entre lcores: 1 porta por lcore (igual l2fwd padrão) */
    rx_lcore_id = 0;
    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0) {
            printf("Ignorando porta %u\n", portid);
            continue;
        }
        while (rte_lcore_is_enabled(rx_lcore_id) == 0 ||
               lcore_queue_conf[rx_lcore_id].n_rx_port == 1) {
            rx_lcore_id++;
            if (rx_lcore_id >= RTE_MAX_LCORE)
                rte_exit(EXIT_FAILURE, "lcores insuficientes para as portas\n");
        }
        if (qconf != &lcore_queue_conf[rx_lcore_id]) {
            qconf = &lcore_queue_conf[rx_lcore_id];
            nb_lcores++;
        }
        qconf->rx_port_list[qconf->n_rx_port] = portid;
        qconf->n_rx_port++;
        printf("Porta %u → lcore %u\n", portid, rx_lcore_id);
        nb_ports_available++;
    }

    if (nb_ports_available == 0)
        rte_exit(EXIT_FAILURE, "Nenhuma porta habilitada\n");

    /* ── fast pool (fill ring, LLC-residente) ────────────────────────────── */
    {
        unsigned min_fast = nb_ports_available * rx_ring_size
                          + nb_lcores * MEMPOOL_CACHE_SIZE
                          + MAX_PKT_BURST * 4;
        if (nb_fast_mbufs < min_fast) {
            printf("AVISO: --fast-mbufs %u < mínimo %u; ajustado.\n",
                   nb_fast_mbufs, min_fast);
            nb_fast_mbufs = min_fast;
        }
        unsigned cache = RTE_MIN((unsigned)MEMPOOL_CACHE_SIZE, nb_fast_mbufs / 8);
        fast_pool = rte_pktmbuf_pool_create(
            "fast_pool", nb_fast_mbufs, cache,
            0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (fast_pool == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao criar fast_pool: %s\n",
                     rte_strerror(rte_errno));
        printf("fast_pool : %u mbufs (~%.1f MiB)\n", nb_fast_mbufs,
               (double)nb_fast_mbufs * RTE_MBUF_DEFAULT_BUF_SIZE / (1 << 20));
    }

    /* ── burst pool (overflow em DRAM) — só se não --disable-burst ────────── */
    if (!disable_burst) {
        unsigned cache = RTE_MIN((unsigned)MEMPOOL_CACHE_SIZE, nb_burst_mbufs / 8);
        burst_pool = rte_pktmbuf_pool_create(
            "burst_pool", nb_burst_mbufs, cache,
            0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (burst_pool == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao criar burst_pool: %s\n",
                     rte_strerror(rte_errno));
        printf("burst_pool: %u mbufs (~%.1f MiB)\n", nb_burst_mbufs,
               (double)nb_burst_mbufs * RTE_MBUF_DEFAULT_BUF_SIZE / (1 << 20));

        RTE_ETH_FOREACH_DEV(portid) {
            if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
                continue;
            char name[32];
            snprintf(name, sizeof(name), "burst_ring_%u", portid);
            dr_burst_ring[portid] = rte_ring_create(
                name, DR_BURST_RING_ENTRIES, rte_socket_id(),
                RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (dr_burst_ring[portid] == NULL)
                rte_exit(EXIT_FAILURE, "Falha ao criar burst_ring porta %u: %s\n",
                         portid, rte_strerror(rte_errno));
        }
    } else {
        printf("burst_pool: DESABILITADO (modo privRing)\n");
    }

    /* ── Work buffer (WorkPackage) em hugepages ───────────────────────────── */
    if (work_per_pkt > 0 && work_buf_sz > 0) {
        work_buf = (uint8_t *)rte_malloc("work_buf", work_buf_sz,
                                         RTE_CACHE_LINE_SIZE);
        if (work_buf == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao alocar work_buf (%zu MiB)\n",
                     (size_t)(work_buf_sz >> 20));
        for (uint64_t k = 0; k < work_buf_sz; k += 4096)
            work_buf[k] = (uint8_t)k;
        printf("work_buf  : %.0f MiB em hugepages, %u acessos/pkt\n",
               (double)work_buf_sz / (1 << 20), work_per_pkt);
    }

    /* ── Inicialização das portas ─────────────────────────────────────────── */
    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
            continue;
        l2fwd_port_init(portid);
    }

    /* ── TX buffers por (lcore, dst_port) ─────────────────────────────────── */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        qconf = &lcore_queue_conf[lcore_id];
        for (unsigned k = 0; k < qconf->n_rx_port; k++) {
            portid = l2fwd_dst_ports[qconf->rx_port_list[k]];
            if (qconf->tx_buffer[portid] != NULL)
                continue;
            qconf->tx_buffer[portid] =
                rte_zmalloc_socket("tx_buffer",
                    RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST),
                    0, rte_eth_dev_socket_id(portid));
            if (qconf->tx_buffer[portid] == NULL)
                rte_exit(EXIT_FAILURE,
                    "Falha ao alocar tx_buffer lcore %u porta %u\n",
                    lcore_id, portid);
            rte_eth_tx_buffer_init(qconf->tx_buffer[portid], MAX_PKT_BURST);
            ret = rte_eth_tx_buffer_set_err_callback(
                qconf->tx_buffer[portid],
                rte_eth_tx_buffer_count_callback,
                &port_statistics[portid].dropped);
            if (ret < 0)
                rte_exit(EXIT_FAILURE, "Falha ao configurar tx_buffer callback\n");
        }
    }
    /* tx_buffer também para o main lcore (se tiver portas) */
    qconf = &lcore_queue_conf[rte_get_main_lcore()];
    for (unsigned k = 0; k < qconf->n_rx_port; k++) {
        portid = l2fwd_dst_ports[qconf->rx_port_list[k]];
        if (qconf->tx_buffer[portid] != NULL)
            continue;
        qconf->tx_buffer[portid] =
            rte_zmalloc_socket("tx_buffer",
                RTE_ETH_TX_BUFFER_SIZE(MAX_PKT_BURST),
                0, rte_eth_dev_socket_id(portid));
        if (qconf->tx_buffer[portid] == NULL)
            rte_exit(EXIT_FAILURE,
                "Falha ao alocar tx_buffer main lcore porta %u\n", portid);
        rte_eth_tx_buffer_init(qconf->tx_buffer[portid], MAX_PKT_BURST);
        ret = rte_eth_tx_buffer_set_err_callback(
            qconf->tx_buffer[portid],
            rte_eth_tx_buffer_count_callback,
            &port_statistics[portid].dropped);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "Falha ao configurar tx_buffer callback\n");
    }

    check_all_ports_link_status(l2fwd_enabled_port_mask);

    /* ── Lança workers e roda main loop ───────────────────────────────────── */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_queue_conf[lcore_id].n_rx_port == 0)
            continue;
        ret = rte_eal_remote_launch(l2fwd_launch_one_lcore, NULL, lcore_id);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eal_remote_launch erro %d\n", ret);
    }

    l2fwd_main_loop();
    rte_eal_mp_wait_lcore();

    print_stats();

    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
            continue;
        printf("Encerrando porta %u...\n", portid);
        ret = rte_eth_dev_stop(portid);
        if (ret != 0)
            printf("rte_eth_dev_stop porta %u: %s\n", portid, strerror(-ret));
        rte_eth_dev_close(portid);
    }

    rte_eal_cleanup();
    printf("Encerrado.\n");
    return 0;
}
