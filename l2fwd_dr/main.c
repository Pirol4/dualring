/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2010-2016 Intel Corporation
 *
 * l2fwd_dr.c  — l2fwd com Dual Receive Ring (TCC DualRing / PPGCC-UFMG)
 *
 * Baseado em dpdk/examples/l2fwd/main.c (DPDK 22.11 LTS).
 * TX path IDÊNTICO ao l2fwd original (rte_eth_tx_buffer + drain 100 µs).
 * Única diferença: gerenciamento do fill ring de recepção com dois pools,
 * modelado no fill/completion ring do AF_XDP.
 *
 * Mapeamento AF_XDP → DPDK (l2fwd_dr):
 *
 *  AF_XDP          | l2fwd_dr
 * -----------------+-------------------------------------------------------
 *  UMEM            | hugepages backing dos mempools
 *  fill ring       | fast_pool  — pequeno, LLC-residente (DDIO-friendly)
 *  RX ring         | fila RX da NIC  (rte_eth_rx_burst)
 *  TX ring         | fila TX da NIC  (rte_eth_tx_buffer + drain)
 *  completion ring | burst_ring — pacotes spillados aguardam TX em DRAM
 *
 * Comportamento dinâmico (RX path, gatilho: watermark do fast pool):
 *
 *   fast_avail >= spill_watermark  →  FAST PATH  (igual l2fwd)
 *     - MAC update + rte_eth_tx_buffer
 *     - drena burst_ring pendente
 *
 *   fast_avail <  spill_watermark  →  SPILL PATH
 *     (a) copia mbuf fast → mbuf burst (preserva o dado do pacote)
 *     (b) libera mbuf fast IMEDIATAMENTE → NIC repõe fill ring
 *     (c) mbuf burst enfileirado no burst_ring → TX diferido
 *
 * Uso (mesmo que l2fwd, mais --fast-mbufs, --burst-mbufs, --spill-watermark):
 *   sudo ./l2fwd_dr -l 0-1 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 \
 *       -- -p 0x3 -T 2
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

#define RTE_LOGTYPE_L2FWD   RTE_LOGTYPE_USER1
#define MAX_PKT_BURST       32
#define BURST_TX_DRAIN_US   100     /* drain TX a cada ~100 µs (idêntico l2fwd) */
#define MEMPOOL_CACHE_SIZE  256
#define MAX_RX_QUEUE_PER_LCORE 16
#define TX_RING_SIZE        1024

/* Verificação de link */
#define CHECK_INTERVAL_MS   100
#define MAX_CHECK_TIME      90  /* 9 s */

/* ── Constantes Dual Ring ──────────────────────────────────────────────────── */

#define DR_BURST_RING_ENTRIES  16384   /* potência-de-2; DRAM */
#define DR_DRAIN_BATCH         32      /* mbufs drenados por iteração */

/* ── Configuração CLI ─────────────────────────────────────────────────────── */

/* l2fwd original */
static uint32_t l2fwd_enabled_port_mask = 0;
static bool     mac_updating             = true;
static int      timer_period             = 10;  /* segundos; 0 = off */

/* DR: novos parâmetros */
static unsigned nb_fast_mbufs    = 4096;   /* fill ring pequeno (LLC)   */
static unsigned nb_burst_mbufs   = 65536;  /* overflow pool (DRAM)      */
static unsigned spill_watermark  = 256;    /* threshold do fill ring     */
static uint16_t rx_ring_size     = 1024;   /* descritores RX da NIC     */
static bool     disable_burst    = false;  /* --disable-burst: só fast pool (small privRing) */

/* DR: simulação de workload (réplica do WorkPackage do ShRing/FastClick) */
static uint8_t  *work_buf        = NULL;
static uint64_t  work_buf_sz     = 0;     /* bytes; 0 = desabilitado     */
static unsigned  work_per_pkt    = 0;     /* acessos aleatórios por pkt  */

/* ── Estado global ────────────────────────────────────────────────────────── */

static volatile bool force_quit = false;

/* Mapeamento portid → porta de destino */
static uint32_t l2fwd_dst_ports[RTE_MAX_ETHPORTS];

/* MACs das portas */
static struct rte_ether_addr l2fwd_ports_eth_addr[RTE_MAX_ETHPORTS];

/* DR: pools compartilhados entre portas.
 * Compartilhado necessário para mlx5 bifurcado: o rx_queue_setup de cada
 * porta registra o mesmo bloco de hugepages em todos os PDs, tornando os
 * mbufs válidos para TX cross-port (exatamente como o l2fwd faz). */
static struct rte_mempool *fast_pool  = NULL;
static struct rte_mempool *burst_pool = NULL;

/* DR: burst ring por porta de ingresso (SP/SC: só 1 lcore por porta) */
static struct rte_ring *dr_burst_ring[RTE_MAX_ETHPORTS];

/* Timer em ciclos (convertido de segundos em main) */
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
    uint64_t dropped;
    /* DR: contadores adicionais */
    uint64_t spilled;       /* fast→burst ring                */
    uint64_t drained;       /* burst ring→TX                  */
    uint64_t drop_spill;    /* descartados no spill           */
    /* Medição de desempenho: ciclos por pacote (rdtsc) */
    uint64_t cycles_total;  /* soma de ciclos gastos no RX path */
    uint64_t cycles_count;  /* pacotes processados             */
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

        uint64_t ctotal = port_statistics[portid].cycles_total;
        uint64_t ccount = port_statistics[portid].cycles_count;
        uint64_t cpkt   = (ccount > 0) ? (ctotal / ccount) : 0;

        printf("\n Porta %u ────────────────────────────────────────\n", portid);
        printf("  rx          : %16" PRIu64 "\n", port_statistics[portid].rx);
        printf("  tx          : %16" PRIu64 "\n", port_statistics[portid].tx);
        printf("  dropped     : %16" PRIu64 "\n", port_statistics[portid].dropped);
        printf("  [DR] spilled    : %12" PRIu64 "  (fast→burst ring)\n",
               port_statistics[portid].spilled);
        printf("  [DR] drained    : %12" PRIu64 "  (burst ring→TX)\n",
               port_statistics[portid].drained);
        printf("  [DR] drop_spill : %12" PRIu64 "\n",
               port_statistics[portid].drop_spill);
        printf("  [DR] fast_avail : %12u  (de %u mbufs)\n",
               fast_avail, nb_fast_mbufs);
        printf("  [DR] cycles/pkt : %12" PRIu64 "  (%.1f Mpps@%.0fGHz)\n",
               cpkt,
               cpkt > 0 ? (rte_get_tsc_hz() / 1e6 / cpkt) : 0.0,
               rte_get_tsc_hz() / 1e9);

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
    printf("  dropped     : %16" PRIu64 "\n", total_dropped);
    printf("  [DR] spilled    : %12" PRIu64 "\n", total_spilled);
    printf("  [DR] drained    : %12" PRIu64 "\n", total_drained);
    printf("  [DR] drop_spill : %12" PRIu64 "\n", total_drop_spill);
    printf("═════════════════════════════════════════════════════\n");
}

/* ── Simulação de workload (WorkPackage — réplica do ShRing/FastClick) ────── */

/* Faz 'work_per_pkt' acessos aleatórios a 'work_buf' de tamanho 'work_buf_sz'.
 * Simula processamento com footprint de memória configurável, como NAT/firewall.
 * A semente varia por pacote para evitar prefetch especulativo do HW. */
static inline void
do_work(struct rte_mbuf *pkt)
{
    if (likely(work_per_pkt == 0 || work_buf == NULL))
        return;
    uint32_t seed = rte_be_to_cpu_32(
        *rte_pktmbuf_mtod_offset(pkt, uint32_t *, offsetof(struct rte_ether_hdr, src_addr)));
    for (unsigned k = 0; k < work_per_pkt; k++) {
        seed = seed * 1664525u + 1013904223u;  /* LCG — distribui uniformemente */
        volatile uint8_t *p = work_buf + (seed % work_buf_sz);
        (void)*p;
    }
}

/* ── MAC update (idêntico l2fwd) ─────────────────────────────────────────── */

static void
l2fwd_mac_updating(struct rte_mbuf *m, unsigned dest_portid)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);

    /* dst: 02:00:00:00:00:XX  (locally-administered, passa pelo DMAC filter
     * do mlx5 bifurcado sem precisar de promiscuous) */
    uint8_t *tmp = &eth->dst_addr.addr_bytes[0];
    *((uint64_t *)tmp) = 0x000000000002ULL + ((uint64_t)dest_portid << 40);

    rte_ether_addr_copy(&l2fwd_ports_eth_addr[dest_portid], &eth->src_addr);
}

/* ── Fast path: forward direto (idêntico l2fwd) ──────────────────────────── */

static void
l2fwd_simple_forward(struct rte_mbuf *m, unsigned portid,
                     struct lcore_queue_conf *qconf)
{
    unsigned dst_port = l2fwd_dst_ports[portid];

    do_work(m);   /* simulação de workload (NOP se work_per_pkt == 0) */

    if (mac_updating)
        l2fwd_mac_updating(m, dst_port);

    int sent = rte_eth_tx_buffer(dst_port, 0, qconf->tx_buffer[dst_port], m);
    if (sent)
        port_statistics[dst_port].tx += (uint64_t)sent;
}

/* ── DR: spill path ──────────────────────────────────────────────────────── */

/*
 * fill ring sob pressão: copia pkts[] (mbufs do fast_pool) para o burst_pool
 * e enfileira no burst_ring. Os mbufs fast são liberados IMEDIATAMENTE para
 * que a NIC possa reenviar os descritores RX sem aguardar TX.
 * O MAC já é atualizado aqui (conhecemos portid) para simplificar a drenagem.
 */
static void
dr_spill_pkts(struct rte_mbuf **pkts, uint16_t n, unsigned portid)
{
    unsigned dst_port = l2fwd_dst_ports[portid];

    for (uint16_t i = 0; i < n; i++) {
        struct rte_mbuf *copy =
            rte_pktmbuf_copy(pkts[i], burst_pool, 0, UINT32_MAX);

        rte_pktmbuf_free(pkts[i]);   /* devolve fast mbuf → fill ring reposto */

        if (unlikely(copy == NULL)) {
            port_statistics[portid].drop_spill++;
            continue;
        }

        if (mac_updating)
            l2fwd_mac_updating(copy, dst_port);

        if (unlikely(rte_ring_enqueue(dr_burst_ring[portid], copy) != 0)) {
            rte_pktmbuf_free(copy);
            port_statistics[portid].drop_spill++;
            continue;
        }
        port_statistics[portid].spilled++;
    }
}

/* ── DR: drena burst ring → TX ───────────────────────────────────────────── */

static void
dr_drain_burst_ring(unsigned portid, struct lcore_queue_conf *qconf)
{
    void *burst_pkts[DR_DRAIN_BATCH];
    unsigned dst_port = l2fwd_dst_ports[portid];

    unsigned n = rte_ring_dequeue_burst(dr_burst_ring[portid],
                                         burst_pkts, DR_DRAIN_BATCH, NULL);
    if (n == 0)
        return;

    for (unsigned i = 0; i < n; i++) {
        /* MAC já foi atualizado durante o spill */
        int sent = rte_eth_tx_buffer(dst_port, 0, qconf->tx_buffer[dst_port],
                                      (struct rte_mbuf *)burst_pkts[i]);
        if (sent)
            port_statistics[dst_port].tx += (uint64_t)sent;
        port_statistics[portid].drained++;
    }
}

/* ── Main loop ───────────────────────────────────────────────────────────── */

static void
l2fwd_main_loop(void)
{
    struct rte_mbuf *pkts_burst[MAX_PKT_BURST];
    unsigned lcore_id;
    uint64_t prev_tsc = 0, diff_tsc, cur_tsc, timer_tsc = 0;
    unsigned i, j, portid, nb_rx;
    struct lcore_queue_conf *qconf;

    /* drain TX a cada BURST_TX_DRAIN_US µs (idêntico l2fwd) */
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

            if (unlikely(nb_rx == 0))
                continue;

            port_statistics[portid].rx += nb_rx;

            uint64_t t0 = rte_rdtsc();

            if (disable_burst) {
                /*
                 * MODO SMALL PRIVRING (baseline de comparação):
                 * Apenas fast_pool, sem burst ring. Equivale ao "small privRing"
                 * do ShRing paper — eficiente em LLC mas dropa sob burst.
                 */
                for (j = 0; j < nb_rx; j++) {
                    if (j + 1 < nb_rx)
                        rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j + 1],
                                                        void *));
                    l2fwd_simple_forward(pkts_burst[j], portid, qconf);
                }
            } else {
                /* ── Decisão do caminho (gatilho: fill ring pressure) ──── */
                unsigned fast_avail = rte_mempool_avail_count(fast_pool);

                /*
                 * Drena o burst ring independente do caminho: sem isso, o burst
                 * ring enche e o burst_pool esgota quando ficamos presos no
                 * spill path por múltiplos ciclos consecutivos.
                 */
                dr_drain_burst_ring(portid, qconf);

                if (unlikely(fast_avail < spill_watermark)) {
                    /*
                     * SPILL PATH: fill ring sob pressão.
                     * Copia para burst_pool e libera fast mbufs imediatamente;
                     * o drain acima já abriu espaço no burst ring e burst_pool.
                     */
                    dr_spill_pkts(pkts_burst, (uint16_t)nb_rx, portid);
                } else {
                    /*
                     * FAST PATH: fill ring saudável.
                     * Forward direto (igual l2fwd).
                     */
                    for (j = 0; j < nb_rx; j++) {
                        if (j + 1 < nb_rx)
                            rte_prefetch0(rte_pktmbuf_mtod(pkts_burst[j + 1],
                                                            void *));
                        l2fwd_simple_forward(pkts_burst[j], portid, qconf);
                    }
                }
            }

            uint64_t t1 = rte_rdtsc();
            port_statistics[portid].cycles_total += (t1 - t0);
            port_statistics[portid].cycles_count += nb_rx;
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

    /* DR: fila RX alimentada pelo fast_pool (fill ring pequeno) */
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
        "  -p PORTMASK          portas em hex (padrão: todas)\n"
        "  --no-mac-updating    desabilita MAC update\n"
        "  -T SEGUNDOS          intervalo de stats (padrão %d; 0=off)\n"
        "\n"
        "  Dual Ring (DR):\n"
        "  --fast-mbufs N       tamanho do fill ring/fast pool (padrão %u)\n"
        "  --burst-mbufs N      tamanho do burst pool/DRAM (padrão %u)\n"
        "  --spill-watermark N  threshold: spill quando fast_avail < N "
        "(padrão %u)\n"
        "  --rx-ring-size N     descritores RX da NIC (padrão %u; pot-de-2)\n"
        "  --disable-burst      desabilita burst ring (modo small privRing)\n"
        "\n"
        "  Workload (WorkPackage, réplica do ShRing):\n"
        "  --work-mem-mb M      tamanho do buffer de trabalho em MiB (padrão: 0=off)\n"
        "  --work-per-pkt N     acessos aleatórios ao buffer por pacote (padrão: 0)\n",
        prgname, timer_period, nb_fast_mbufs, nb_burst_mbufs, spill_watermark,
        rx_ring_size);
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
        { "no-mac-updating",  no_argument,       NULL, 0 },
        { "fast-mbufs",       required_argument, NULL, 0 },
        { "burst-mbufs",      required_argument, NULL, 0 },
        { "spill-watermark",  required_argument, NULL, 0 },
        { "rx-ring-size",     required_argument, NULL, 0 },
        { "disable-burst",    no_argument,       NULL, 0 },
        { "work-mem-mb",      required_argument, NULL, 0 },
        { "work-per-pkt",     required_argument, NULL, 0 },
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
            else if (!strcmp(lgopts[option_index].name, "spill-watermark"))
                spill_watermark = (unsigned)atoi(optarg);
            else if (!strcmp(lgopts[option_index].name, "rx-ring-size"))
                rx_ring_size = (uint16_t)atoi(optarg);
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

    /* ── EAL ─────────────────────────────────────────────────────────────── */
    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Erro na inicialização EAL\n");
    argc -= ret;
    argv += ret;

    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    /* ── Argumentos do app ───────────────────────────────────────────────── */
    ret = l2fwd_parse_args(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Argumentos inválidos\n");

    printf("MAC updating   : %s\n", mac_updating ? "sim" : "não");
    printf("Stats timer    : %d s\n", timer_period);
    printf("fast-mbufs     : %u\n", nb_fast_mbufs);
    printf("burst-mbufs    : %u%s\n", nb_burst_mbufs,
           disable_burst ? " (DESABILITADO -- small privRing)" : "");
    printf("spill-watermark: %u\n", spill_watermark);
    printf("rx-ring-size   : %u\n", rx_ring_size);
    printf("disable-burst  : %s\n", disable_burst ? "sim" : "não");
    printf("work-mem-mb    : %u MiB\n", (unsigned)(work_buf_sz >> 20));
    printf("work-per-pkt   : %u\n", work_per_pkt);

    if (timer_period > 0)
        timer_period_tsc = (uint64_t)timer_period * rte_get_tsc_hz();

    /* ── Portas ──────────────────────────────────────────────────────────── */
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

    /* Distribui portas entre lcores: 1 porta por lcore, idêntico ao l2fwd padrão */
    rx_lcore_id = 0;
    RTE_ETH_FOREACH_DEV(portid) {
        if ((l2fwd_enabled_port_mask & (1u << portid)) == 0) {
            printf("Ignorando porta %u\n", portid);
            continue;
        }
        /* Avança para o próximo lcore habilitado que ainda não tem porta */
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

    /* ── DR: cria fast pool (fill ring, LLC-residente) ───────────────────── */
    {
        /* Garante mbufs suficientes: nb_rxd × nb_ports + caches + burst */
        unsigned min_fast = nb_ports_available * rx_ring_size
                            + nb_lcores * MEMPOOL_CACHE_SIZE
                            + MAX_PKT_BURST * 4;
        if (nb_fast_mbufs < min_fast) {
            printf("AVISO: --fast-mbufs %u < mínimo %u; ajustado.\n",
                   nb_fast_mbufs, min_fast);
            nb_fast_mbufs = min_fast;
        }
        unsigned cache = RTE_MIN((unsigned)MEMPOOL_CACHE_SIZE,
                                  nb_fast_mbufs / 8);
        fast_pool = rte_pktmbuf_pool_create(
            "fast_pool", nb_fast_mbufs, cache,
            0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (fast_pool == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao criar fast_pool: %s\n",
                     rte_strerror(rte_errno));
        printf("fast_pool : %u mbufs (~%.1f MiB)\n", nb_fast_mbufs,
               (double)nb_fast_mbufs * RTE_MBUF_DEFAULT_BUF_SIZE / (1<<20));
    }

    /* ── DR: cria burst pool (overflow, DRAM) — apenas se não --disable-burst */
    if (!disable_burst) {
        unsigned cache = RTE_MIN((unsigned)MEMPOOL_CACHE_SIZE,
                                  nb_burst_mbufs / 8);
        burst_pool = rte_pktmbuf_pool_create(
            "burst_pool", nb_burst_mbufs, cache,
            0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
        if (burst_pool == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao criar burst_pool: %s\n",
                     rte_strerror(rte_errno));
        printf("burst_pool: %u mbufs (~%.1f MiB)\n", nb_burst_mbufs,
               (double)nb_burst_mbufs * RTE_MBUF_DEFAULT_BUF_SIZE / (1<<20));
    } else {
        printf("burst_pool: DESABILITADO (small privRing mode)\n");
    }

    /* ── DR: cria burst rings por porta de ingresso ──────────────────────── */
    if (!disable_burst) {
        RTE_ETH_FOREACH_DEV(portid) {
            if ((l2fwd_enabled_port_mask & (1u << portid)) == 0)
                continue;
            char name[32];
            snprintf(name, sizeof(name), "burst_ring_%u", portid);
            dr_burst_ring[portid] = rte_ring_create(name, DR_BURST_RING_ENTRIES,
                                                      rte_socket_id(),
                                                      RING_F_SP_ENQ | RING_F_SC_DEQ);
            if (dr_burst_ring[portid] == NULL)
                rte_exit(EXIT_FAILURE, "Falha ao criar burst_ring porta %u: %s\n",
                         portid, rte_strerror(rte_errno));
        }
    }

    /* ── Work buffer (WorkPackage): alocado em hugepages para reproduzir
     *    footprint de memória de NFs reais (ShRing Fig. 8) ─────────────────── */
    if (work_per_pkt > 0 && work_buf_sz > 0) {
        work_buf = (uint8_t *)rte_malloc("work_buf", work_buf_sz,
                                          RTE_CACHE_LINE_SIZE);
        if (work_buf == NULL)
            rte_exit(EXIT_FAILURE, "Falha ao alocar work_buf (%zu MiB)\n",
                     (size_t)(work_buf_sz >> 20));
        /* Toca cada página para garantir alocação física antes dos experimentos */
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

    /* ── TX buffers por (lcore, dst_port) ───────────────────────────────── */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        qconf = &lcore_queue_conf[lcore_id];
        for (unsigned i = 0; i < qconf->n_rx_port; i++) {
            portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
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
    /* Também aloca tx_buffer para o main lcore (se tiver portas) */
    qconf = &lcore_queue_conf[rte_get_main_lcore()];
    for (unsigned i = 0; i < qconf->n_rx_port; i++) {
        portid = l2fwd_dst_ports[qconf->rx_port_list[i]];
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

    /* ── Lança workers e roda main loop ─────────────────────────────────── */
    RTE_LCORE_FOREACH_WORKER(lcore_id) {
        if (lcore_queue_conf[lcore_id].n_rx_port == 0)
            continue;
        ret = rte_eal_remote_launch(l2fwd_launch_one_lcore, NULL, lcore_id);
        if (ret < 0)
            rte_exit(EXIT_FAILURE, "rte_eal_remote_launch erro %d\n", ret);
    }

    l2fwd_main_loop();       /* main lcore também trabalha */
    rte_eal_mp_wait_lcore();

    print_stats();

    /* ── Encerra portas ──────────────────────────────────────────────────── */
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
