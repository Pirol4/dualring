/* SPDX-License-Identifier: BSD-3-Clause
 *
 * dual_ring_fwd.c — MVP 1 do TCC: encaminhador L2 com anéis duplos de recepção
 *
 * Arquitetura (inspirada no modelo fill/completion do AF_XDP, implementada
 * 100% sobre DPDK):
 *
 *   - FAST POOL : rte_mempool pequeno, dimensionado para residir na LLC.
 *                 É o pool de onde a NIC aloca buffers de RX (DDIO escreve
 *                 direto na cache). Reciclagem rápida => working set quente.
 *
 *   - BURST RING: rte_ring + rte_mempool grande em DRAM. Quando o fast pool
 *                 fica sob pressão (rajada), os pacotes são copiados para
 *                 buffers do burst pool e enfileirados para processamento
 *                 diferido — liberando imediatamente os buffers fast e
 *                 mantendo o working set da NIC pequeno (mitiga leaky DMA).
 *
 * Datapath (por porta, single core no MVP):
 *   1. rx_burst() da fila 0 (alimentada pelo fast pool)
 *   2. se fast pool sob pressão (avail < spill_watermark) => SPILL:
 *        copia mbufs para o burst pool, enfileira no burst ring,
 *        libera os buffers fast na hora
 *      senão => FORWARD direto (swap de MAC + tx na porta par)
 *   3. drena o burst ring quando a pressão cede
 *
 * MVP 1 = particionamento ESTÁTICO (watermark fixo via CLI).
 * MVP 2 = gatilho DINÂMICO por LLC-load-misses via perf_event_open:
 *   Uma thread de monitor lê os contadores de hardware (PERF_TYPE_HW_CACHE /
 *   PERF_COUNT_HW_CACHE_LL) no CPU do datapath a cada --monitor-ms ms e
 *   calcula a taxa de misses = misses / loads. Quando a taxa ultrapassa
 *   --miss-hi, ativa o burst path (g_llc_spill_active=1). Quando cai abaixo
 *   de --miss-lo, volta ao fast path (histerese para evitar oscilação).
 *
 * Uso:
 *   dual_ring_fwd [EAL opts] -- [--fast-mbufs N] [--burst-mbufs N]
 *                               [--spill-watermark N] [--force-spill] [-T seg]
 *
 * Dimensionamento p/ AMD EPYC 7402P (CloudLab d6515):
 *   LLC = 128 MiB no total, organizada em CCXs de 16 MiB.
 *   fast pool padrão: 2048 mbufs × ~2.2 KiB ≈ 4.4 MiB  (cabe folgado num CCX)
 *   burst pool padrão: 65536 mbufs ≈ 140 MiB            (DRAM)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/syscall.h>
#include <linux/perf_event.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_ring.h>
#include <rte_ether.h>
#include <rte_lcore.h>
#include <rte_cycles.h>

#define RX_RING_SIZE   1024
#define TX_RING_SIZE   1024
#define BURST_SIZE       32
#define DRAIN_BATCH      32

/* ─── Configuração (CLI) ─────────────────────────────────────────────────── */

static unsigned int fast_mbufs       = 8192;   /* tamanho do fast pool — deve ser >> nb_rxd */
static unsigned int burst_mbufs      = 65536;  /* tamanho do burst pool (DRAM)*/
static unsigned int burst_ring_size  = 16384;  /* capacidade do burst ring    */
static unsigned int spill_watermark  = 256;    /* avail mínimo do fast pool   */
static unsigned int stats_period_s   = 0;      /* -T: período de stats (0=off)*/
static bool         force_spill      = false;  /* p/ teste: spill sempre      */

/* ─── MVP2: configuração do monitor LLC ─────────────────────────────────── */

static double        llc_hi_threshold = 0.05;  /* taxa de miss > 5%: ativa burst path   */
static double        llc_lo_threshold = 0.02;  /* taxa de miss < 2%: volta ao fast path */
static unsigned int  monitor_ms       = 100;    /* intervalo de polling do perf, em ms   */

/* ─── Estado global ──────────────────────────────────────────────────────── */

static volatile bool     force_quit         = false;

/* MVP2: estado do monitor LLC (escritos pela monitor thread, lidos pelo datapath) */
static _Atomic uint32_t  g_llc_spill_active = 0;  /* 1 = burst path ativa       */
static _Atomic uint32_t  g_llc_miss_ppm     = 0;  /* miss rate × 1e6 (p/ stats) */
static int               g_worker_cpu       = -1; /* CPU do datapath             */

/* Pool único compartilhado por todas as portas.
 * Com pools por porta, cada rte_eth_rx_queue_setup registra o MR apenas no
 * Protection Domain da sua porta. Ao fazer TX cross-port (port 0 → port 1)
 * o hardware recebe um lkey inválido e descarta silenciosamente os pacotes.
 * Um pool compartilhado faz o rx_queue_setup de cada porta registrar o mesmo
 * bloco de memória em ambos os PDs, tornando os mbufs válidos para TX em
 * qualquer porta — exatamente o que o l2fwd faz. */
static struct rte_mempool *shared_fast_pool;

struct port_ctx {
    struct rte_mempool *fast_pool;   /* aponta para shared_fast_pool */
    struct rte_mempool *burst_pool;
    struct rte_ring    *burst_ring;
    uint16_t            tx_port;     /* porta de saída (par: 0<->1)          */
};

struct dr_stats {
    uint64_t rx_total;
    uint64_t fwd_fast;       /* encaminhados direto (caminho rápido)         */
    uint64_t spilled;        /* desviados para o burst ring                  */
    uint64_t fwd_burst;      /* drenados do burst ring e encaminhados        */
    uint64_t drop_spill;     /* perdidos no spill (pool/ring cheios)         */
    uint64_t drop_tx;        /* não couberam no TX                           */
};

static struct port_ctx g_ctx[RTE_MAX_ETHPORTS];
static struct dr_stats g_stats[RTE_MAX_ETHPORTS];

/* ─── Sinais e stats ─────────────────────────────────────────────────────── */

static void
signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
        force_quit = true;
}

static void
print_stats(void)
{
    uint16_t port;

    printf("\n══════════════ dual_ring_fwd: estatísticas ══════════════\n");
    RTE_ETH_FOREACH_DEV(port) {
        const struct dr_stats *s = &g_stats[port];
        unsigned int avail = g_ctx[port].fast_pool ?
            rte_mempool_avail_count(g_ctx[port].fast_pool) : 0;

        printf("Porta %u:\n", port);
        printf("  rx_total    : %12" PRIu64 "\n", s->rx_total);
        printf("  fwd_fast    : %12" PRIu64 "  (caminho rápido)\n", s->fwd_fast);
        printf("  spilled     : %12" PRIu64 "  (desviados p/ burst ring)\n", s->spilled);
        printf("  fwd_burst   : %12" PRIu64 "  (drenados do burst ring)\n", s->fwd_burst);
        printf("  drop_spill  : %12" PRIu64 "\n", s->drop_spill);
        printf("  drop_tx     : %12" PRIu64 "\n", s->drop_tx);
        printf("  fast_avail  : %12u  (de %u mbufs)\n", avail, fast_mbufs);
    }
    /* MVP2: exibe estado do monitor LLC */
    if (g_worker_cpu >= 0 && monitor_ms > 0) {
        double miss_rate = atomic_load(&g_llc_miss_ppm) / 1e6;
        printf("LLC miss rate : %8.4f%%  [%s]\n",
               miss_rate * 100.0,
               atomic_load(&g_llc_spill_active)
                   ? "BURST PATH ativa (LLC sob pressão)"
                   : "FAST PATH ativa");
    }
    printf("══════════════════════════════════════════════════════════\n");
}

/* ─── MVP2: monitor de LLC via perf_event_open ───────────────────────────── */

static long
sys_perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu,
                    int group_fd, unsigned long flags)
{
    return syscall(__NR_perf_event_open, attr, pid, cpu, group_fd, flags);
}

/* Lê o tipo numérico de uma PMU do sysfs (ex: /sys/.../amd_l3/type → 12). */
static int
sysfs_pmu_type(const char *pmu_name)
{
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/type", pmu_name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int type = -1;
    (void)fscanf(f, "%d", &type);
    fclose(f);
    return type;
}

/* Lê o config de um evento de uma PMU do sysfs.
 * Formato típico: "event=0x04,umask=0x08" → devolve (umask<<8)|event */
static uint64_t
sysfs_event_config(const char *pmu_name, const char *event_name)
{
    char path[192];
    snprintf(path, sizeof(path),
             "/sys/bus/event_source/devices/%s/events/%s", pmu_name, event_name);
    FILE *f = fopen(path, "r");
    if (!f) return UINT64_MAX;
    char buf[128] = {0};
    (void)fgets(buf, sizeof(buf), f);
    fclose(f);
    unsigned long event = 0, umask = 0;
    /* suporta "event=0xXX" e "event=0xXX,umask=0xXX" */
    sscanf(buf, "event=%lx,umask=%lx", &event, &umask);
    if (!event) sscanf(buf, "event=%lx", &event);
    return event | (umask << 8);
}

/* Abre um fd de perf_event para cache LLC com cadeia de fallback:
 *   1. PERF_TYPE_HW_CACHE / PERF_COUNT_HW_CACHE_LL  (Intel, genérico)
 *   2. PMU "amd_l3" via sysfs (AMD Zen 2+)
 *   3. PERF_TYPE_HARDWARE CACHE_MISSES/REFS           (proxy genérico)
 *
 * O AMD Zen 2 não expõe LLC via PERF_TYPE_HW_CACHE (retorna ENOENT);
 * o L3 é uma PMU uncore separada ("amd_l3") acessível pelo sysfs. */
static int
open_llc_fd(uint32_t op, uint32_t result)
{
    struct perf_event_attr pe = {
        .size       = sizeof(struct perf_event_attr),
        .disabled   = 0,
        .exclude_hv = 1,
    };
    int fd;

    /* ── Tentativa 1: HW_CACHE genérico (Intel/AMD com suporte de kernel) ── */
    pe.type   = PERF_TYPE_HW_CACHE;
    pe.config = (uint64_t)PERF_COUNT_HW_CACHE_LL |
                ((uint64_t)op     << 8)            |
                ((uint64_t)result << 16);
    fd = (int)sys_perf_event_open(&pe, -1, g_worker_cpu, -1, 0);
    if (fd >= 0) {
        printf("[llc_monitor] PERF_TYPE_HW_CACHE/LL: cpu=%d config=0x%lx\n",
               g_worker_cpu, (unsigned long)pe.config);
        return fd;
    }
    fprintf(stderr, "[llc_monitor] HW_CACHE/LL falhou (errno=%d %s), tentando AMD L3...\n",
            errno, strerror(errno));

    /* ── Tentativa 2: AMD L3 PMU via sysfs ── */
    int amd_type = sysfs_pmu_type("amd_l3");
    if (amd_type > 0) {
        bool is_miss = (result == PERF_COUNT_HW_CACHE_RESULT_MISS);
        uint64_t cfg = is_miss
            ? sysfs_event_config("amd_l3", "l3_cache_miss_all")
            : sysfs_event_config("amd_l3", "l3_cache_accesses_all");

        if (cfg != UINT64_MAX) {
            pe.type   = (uint32_t)amd_type;
            pe.config = cfg;
            /* AMD L3 é uncore: usar cpu=0 do socket, não o worker core */
            fd = (int)sys_perf_event_open(&pe, -1, 0, -1, 0);
            if (fd >= 0) {
                printf("[llc_monitor] AMD L3 PMU: tipo=%d config=0x%lx\n",
                       amd_type, (unsigned long)cfg);
                return fd;
            }
        }
    }

    /* ── Tentativa 3: PERF_TYPE_HARDWARE genérico (proxy L2/LLC) ── */
    pe.type   = PERF_TYPE_HARDWARE;
    pe.config = (result == PERF_COUNT_HW_CACHE_RESULT_MISS)
                    ? PERF_COUNT_HW_CACHE_MISSES
                    : PERF_COUNT_HW_CACHE_REFERENCES;
    fd = (int)sys_perf_event_open(&pe, -1, g_worker_cpu, -1, 0);
    if (fd >= 0) {
        printf("[llc_monitor] Fallback PERF_TYPE_HARDWARE "
               "(proxy L2/cache, não LLC específico)\n");
        return fd;
    }

    perror("[llc_monitor] perf_event_open (todas as tentativas falharam)");
    return -1;
}

static void *
llc_monitor_thread(__rte_unused void *arg)
{
    int fd_miss = open_llc_fd(PERF_COUNT_HW_CACHE_OP_READ,
                               PERF_COUNT_HW_CACHE_RESULT_MISS);
    int fd_load = open_llc_fd(PERF_COUNT_HW_CACHE_OP_READ,
                               PERF_COUNT_HW_CACHE_RESULT_ACCESS);

    if (fd_miss < 0 || fd_load < 0) {
        fprintf(stderr, "[llc_monitor] Falha ao abrir perf events "
                "(precisa de root e kernel >= 3.4)\n");
        if (fd_miss >= 0) close(fd_miss);
        if (fd_load >= 0) close(fd_load);
        return NULL;
    }

    printf("[llc_monitor] CPU %d: hi=%.0f%% lo=%.0f%% intervalo=%ums\n",
           g_worker_cpu, llc_hi_threshold * 100.0,
           llc_lo_threshold * 100.0, monitor_ms);

    uint64_t prev_miss = 0, prev_load = 0;

    while (!force_quit) {
        usleep((unsigned)(monitor_ms * 1000U));

        uint64_t cur_miss, cur_load;
        if (read(fd_miss, &cur_miss, sizeof cur_miss) != (ssize_t)sizeof cur_miss ||
            read(fd_load, &cur_load, sizeof cur_load) != (ssize_t)sizeof cur_load)
            continue;

        uint64_t d_miss = cur_miss - prev_miss;
        uint64_t d_load = cur_load - prev_load;
        prev_miss = cur_miss;
        prev_load = cur_load;

        double rate = (d_load > 0) ? (double)d_miss / (double)d_load : 0.0;
        atomic_store(&g_llc_miss_ppm, (uint32_t)(rate * 1e6));

        uint32_t active = atomic_load(&g_llc_spill_active);
        if (!active && rate > llc_hi_threshold)
            atomic_store(&g_llc_spill_active, 1);
        else if (active && rate < llc_lo_threshold)
            atomic_store(&g_llc_spill_active, 0);
    }

    close(fd_miss);
    close(fd_load);
    return NULL;
}

/* ─── Datapath ───────────────────────────────────────────────────────────── */

/* Reescreve dst MAC igual ao l2fwd (02:00:00:00:00:tx_port).
 * mac_swap simples coloca T-Rex_port0_MAC como dst no port 1; o mlx5
 * bifurcado do T-Rex filtra esse frame no DMAC filter (cross-port).
 * MACs locally-administered (02:xx) não pertencem a nenhuma porta real
 * e passam pelo filtro sem precisar de promiscuous mode. */
static inline void
mac_update(struct rte_mbuf *m, uint16_t tx_port)
{
    struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    uint64_t *dst = (uint64_t *)&eth->dst_addr.addr_bytes[0];
    *dst = 0x000000000002 + ((uint64_t)tx_port << 40);
}

static inline uint16_t
tx_pkts(uint16_t port, struct rte_mbuf **pkts, uint16_t n)
{
    uint16_t sent = rte_eth_tx_burst(port, 0, pkts, n);

    if (unlikely(sent < n)) {
        g_stats[port].drop_tx += n - sent;
        for (uint16_t i = sent; i < n; i++)
            rte_pktmbuf_free(pkts[i]);
    }
    return sent;
}

/* SPILL: copia o conteúdo para um mbuf do burst pool (DRAM), enfileira no
 * burst ring e libera o buffer fast IMEDIATAMENTE — é isso que mantém o
 * working set da NIC pequeno e residente na LLC durante rajadas. */
static inline void
spill_pkts(uint16_t port, struct rte_mbuf **pkts, uint16_t n)
{
    struct port_ctx *ctx = &g_ctx[port];

    for (uint16_t i = 0; i < n; i++) {
        struct rte_mbuf *copy =
            rte_pktmbuf_copy(pkts[i], ctx->burst_pool, 0, UINT32_MAX);

        rte_pktmbuf_free(pkts[i]);          /* devolve o fast buffer já    */

        if (unlikely(copy == NULL)) {
            g_stats[port].drop_spill++;
            continue;
        }
        if (unlikely(rte_ring_enqueue(ctx->burst_ring, copy) != 0)) {
            rte_pktmbuf_free(copy);
            g_stats[port].drop_spill++;
            continue;
        }
        g_stats[port].spilled++;
    }
}

/* Drena até DRAIN_BATCH pacotes do burst ring e encaminha. */
static inline void
drain_burst_ring(uint16_t port)
{
    struct port_ctx *ctx = &g_ctx[port];
    struct rte_mbuf *pkts[DRAIN_BATCH];
    unsigned int n;

    n = rte_ring_dequeue_burst(ctx->burst_ring, (void **)pkts,
                               DRAIN_BATCH, NULL);
    if (n == 0)
        return;

    for (unsigned int i = 0; i < n; i++)
        mac_update(pkts[i], ctx->tx_port);

    uint16_t sent = tx_pkts(ctx->tx_port, pkts, n);
    g_stats[port].fwd_burst += sent;
}

static int
lcore_main(__rte_unused void *arg)
{
    uint16_t port;
    struct rte_mbuf *pkts[BURST_SIZE];
    uint64_t prev_tsc = 0;
    const uint64_t stats_tsc =
        stats_period_s ? (uint64_t)stats_period_s * rte_get_timer_hz() : 0;

    printf("Core %u: datapath iniciado (fast=%u mbufs, burst=%u mbufs, "
           "watermark=%u%s%s)\n",
           rte_lcore_id(), fast_mbufs, burst_mbufs, spill_watermark,
           force_spill ? ", FORCE-SPILL" : "",
           (monitor_ms > 0 && g_worker_cpu >= 0) ? ", LLC-MONITOR" : "");

    while (!force_quit) {
        RTE_ETH_FOREACH_DEV(port) {
            struct port_ctx *ctx = &g_ctx[port];

            uint16_t nb = rte_eth_rx_burst(port, 0, pkts, BURST_SIZE);
            if (nb > 0) {
                g_stats[port].rx_total += nb;

                /* Decisão fast vs spill:
                 *   force_spill           → sempre burst (debug / MVP1 test)
                 *   g_llc_spill_active    → burst ativado pelo monitor LLC (MVP2)
                 *   avail < watermark     → pool esgotando (fallback estático) */
                unsigned int avail =
                    rte_mempool_avail_count(ctx->fast_pool);
                bool under_pressure =
                    force_spill                              ||
                    atomic_load(&g_llc_spill_active)        ||
                    (avail < spill_watermark);

                if (unlikely(under_pressure)) {
                    spill_pkts(port, pkts, nb);
                } else {
                    for (uint16_t i = 0; i < nb; i++)
                        mac_update(pkts[i], ctx->tx_port);
                    uint16_t sent = tx_pkts(ctx->tx_port, pkts, nb);
                    g_stats[port].fwd_fast += sent;
                }
            }

            /* Drena o trabalho diferido — prioridade menor que o RX.      */
            drain_burst_ring(port);
        }

        if (stats_tsc) {
            uint64_t cur_tsc = rte_get_timer_cycles();
            if (cur_tsc - prev_tsc > stats_tsc) {
                print_stats();
                prev_tsc = cur_tsc;
            }
        }
    }
    return 0;
}

/* ─── Inicialização ──────────────────────────────────────────────────────── */

static int
port_init(uint16_t port, uint16_t nb_ports)
{
    struct rte_eth_conf port_conf;
    struct rte_eth_dev_info dev_info;
    char name[64];
    int ret;

    memset(&port_conf, 0, sizeof(port_conf));

    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0)
        return ret;

    /* mq_mode = NONE (padrão): mesma configuração do l2fwd de referência.
     * RSS foi adicionado anteriormente para corrigir 0 pacotes recebidos,
     * mas o problema real era fast_mbufs insuficiente (2048 com nb_rxd=1024
     * deixava headroom zero). Com fast_mbufs=8192 o pool tem espaço e o
     * mlx5 cria as regras de flow steering corretas sem RSS.
     * Manter RSS habilitado altera internamente o QP/SQ do mlx5 de modo que
     * o TX cross-port (porta 0 → porta 1) falha silenciosamente no hardware
     * mesmo com rte_eth_tx_burst retornando sent=n. */
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    struct port_ctx *ctx = &g_ctx[port];
    ctx->tx_port = (nb_ports >= 2) ? (port ^ 1) : port;

    /* FAST POOL — usa o pool compartilhado criado em main().
     * Ao chamar rx_queue_setup com o mesmo pool em todas as portas, o mlx5
     * registra a memória como MR em todos os Protection Domains, tornando
     * os mbufs válidos para TX cross-port sem lkey inválido. */
    ctx->fast_pool = shared_fast_pool;

    /* BURST POOL — grande, DRAM */
    snprintf(name, sizeof(name), "burst_pool_p%u", port);
    ctx->burst_pool = rte_pktmbuf_pool_create(name, burst_mbufs,
        256, 0, RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_eth_dev_socket_id(port));
    if (ctx->burst_pool == NULL)
        rte_exit(EXIT_FAILURE, "Falha ao criar burst pool da porta %u: %s\n",
                 port, rte_strerror(rte_errno));

    /* BURST RING — fila de trabalho diferido (SP/SC no MVP single core) */
    snprintf(name, sizeof(name), "burst_ring_p%u", port);
    ctx->burst_ring = rte_ring_create(name, burst_ring_size,
        rte_eth_dev_socket_id(port), RING_F_SP_ENQ | RING_F_SC_DEQ);
    if (ctx->burst_ring == NULL)
        rte_exit(EXIT_FAILURE, "Falha ao criar burst ring da porta %u: %s\n",
                 port, rte_strerror(rte_errno));

    ret = rte_eth_dev_configure(port, 1, 1, &port_conf);
    if (ret != 0)
        return ret;

    uint16_t nb_rxd = RX_RING_SIZE, nb_txd = TX_RING_SIZE;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (ret != 0)
        return ret;

    /* Garante que o fast pool tem mbufs suficientes para a fila de RX.
     * O PMD pode ter ajustado nb_rxd acima de RX_RING_SIZE; sem headroom
     * suficiente o pool esgota e o hardware descarta todos os pacotes. */
    if ((unsigned int)nb_rxd >= fast_mbufs) {
        rte_exit(EXIT_FAILURE,
            "Porta %u: nb_rxd ajustado para %u >= fast_mbufs %u. "
            "Aumente --fast-mbufs para pelo menos %u.\n",
            port, nb_rxd, fast_mbufs, nb_rxd * 4);
    }
    printf("Porta %u: nb_rxd=%u nb_txd=%u (fast_pool=%u, headroom=%u)\n",
           port, nb_rxd, nb_txd, fast_mbufs, fast_mbufs - nb_rxd);

    /* Fila de RX alimentada pelo FAST pool — este é o ponto-chave         */
    ret = rte_eth_rx_queue_setup(port, 0, nb_rxd,
        rte_eth_dev_socket_id(port), NULL, ctx->fast_pool);
    if (ret < 0)
        return ret;

    ret = rte_eth_tx_queue_setup(port, 0, nb_txd,
        rte_eth_dev_socket_id(port), NULL);
    if (ret < 0)
        return ret;

    ret = rte_eth_dev_start(port);
    if (ret < 0)
        return ret;

    ret = rte_eth_promiscuous_enable(port);
    if (ret != 0 && ret != -ENOTSUP)
        return ret;

    printf("Porta %u inicializada (tx_port=%u, fast=%u, burst=%u, "
           "socket=%d)\n", port, ctx->tx_port, fast_mbufs, burst_mbufs,
           rte_eth_dev_socket_id(port));
    return 0;
}

/* ─── CLI ────────────────────────────────────────────────────────────────── */

static void
usage(const char *prog)
{
    printf("Uso: %s [EAL opts] -- [opções]\n"
           "  --fast-mbufs N        mbufs do fast pool (padrão %u)\n"
           "  --burst-mbufs N       mbufs do burst pool (padrão %u)\n"
           "  --burst-ring N        capacidade do burst ring (padrão %u, pot. de 2)\n"
           "  --spill-watermark N   spill quando fast avail < N (padrão %u)\n"
           "  --force-spill         força o caminho de spill (teste)\n"
           "  --miss-hi F           taxa LLC-miss para ativar burst path (padrão %.2f)\n"
           "  --miss-lo F           taxa LLC-miss para voltar ao fast path (padrão %.2f)\n"
           "  --monitor-ms N        intervalo do monitor LLC em ms (0=desligado, padrão %u)\n"
           "  -T N                  imprime stats a cada N segundos\n",
           prog, fast_mbufs, burst_mbufs, burst_ring_size, spill_watermark,
           llc_hi_threshold, llc_lo_threshold, monitor_ms);
}

static int
parse_args(int argc, char **argv)
{
    static const struct option opts[] = {
        { "fast-mbufs",      required_argument, NULL, 'f' },
        { "burst-mbufs",     required_argument, NULL, 'b' },
        { "burst-ring",      required_argument, NULL, 'r' },
        { "spill-watermark", required_argument, NULL, 'w' },
        { "force-spill",     no_argument,       NULL, 'F' },
        { "miss-hi",         required_argument, NULL, 'H' },
        { "miss-lo",         required_argument, NULL, 'L' },
        { "monitor-ms",      required_argument, NULL, 'm' },
        { NULL, 0, NULL, 0 },
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "T:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'f': fast_mbufs        = (unsigned)atoi(optarg);  break;
        case 'b': burst_mbufs       = (unsigned)atoi(optarg);  break;
        case 'r': burst_ring_size   = (unsigned)atoi(optarg);  break;
        case 'w': spill_watermark   = (unsigned)atoi(optarg);  break;
        case 'F': force_spill       = true;                     break;
        case 'H': llc_hi_threshold  = atof(optarg);            break;
        case 'L': llc_lo_threshold  = atof(optarg);            break;
        case 'm': monitor_ms        = (unsigned)atoi(optarg);  break;
        case 'T': stats_period_s    = (unsigned)atoi(optarg);  break;
        case 'h': usage(argv[0]); exit(0);
        default:  usage(argv[0]); return -1;
        }
    }
    return 0;
}

/* ─── Main ───────────────────────────────────────────────────────────────── */

int
main(int argc, char **argv)
{
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Erro na inicialização do EAL\n");
    argc -= ret;
    argv += ret;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (parse_args(argc, argv) < 0)
        rte_exit(EXIT_FAILURE, "Argumentos inválidos\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "Nenhuma porta Ethernet disponível\n");
    if (nb_ports % 2 != 0)
        printf("AVISO: número ímpar de portas (%u) — última porta ecoa "
               "em si mesma\n", nb_ports);

    /* Pool compartilhado criado antes dos port_init para que o rx_queue_setup
     * de cada porta registre a mesma memória em todos os PDs mlx5. */
    shared_fast_pool = rte_pktmbuf_pool_create(
        "fast_pool_shared", fast_mbufs,
        RTE_MIN(256U, fast_mbufs / 16),   /* cache menor: pool maior, 16 lcores */
        0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (shared_fast_pool == NULL)
        rte_exit(EXIT_FAILURE, "Falha ao criar fast pool compartilhado: %s\n",
                 rte_strerror(rte_errno));

    uint16_t port;
    RTE_ETH_FOREACH_DEV(port) {
        if (port_init(port, nb_ports) != 0)
            rte_exit(EXIT_FAILURE, "Falha ao inicializar porta %u\n", port);
    }

    /* Roda o datapath num worker lcore (CPU isolado pelo isolcpus=1-63), como
     * o l2fwd faz. Lcore 0 (CPU 0) não é isolado e concorre com interrupções
     * do kernel e com o master_thread do T-Rex (que também usa CPU 0 por
     * padrão no trex_cfg.yaml) — isso pode corromper o timing do TX path. */
    unsigned int worker_lcore = rte_get_next_lcore(-1, 1, 0);
    if (worker_lcore >= RTE_MAX_LCORE)
        rte_exit(EXIT_FAILURE,
            "Nenhum worker lcore disponível — use -l 0-1 ou mais.\n");
    g_worker_cpu = (int)rte_lcore_to_cpu_id(worker_lcore);
    printf("Datapath no lcore %u (CPU %d)\n", worker_lcore, g_worker_cpu);

    /* MVP2: inicia monitor LLC na thread principal (lcore 0 / CPU 0).
     * perf_event_open com cpu=g_worker_cpu mede os eventos *naquele* CPU,
     * independente de qual thread faz a leitura. */
    pthread_t monitor_tid;
    bool monitor_running = false;
    if (!force_spill && monitor_ms > 0) {
        if (pthread_create(&monitor_tid, NULL, llc_monitor_thread, NULL) == 0) {
            pthread_detach(monitor_tid);
            monitor_running = true;
        } else {
            perror("pthread_create (llc_monitor)");
        }
    }
    if (!monitor_running)
        printf("[llc_monitor] Desligado (--monitor-ms 0 ou --force-spill)\n");

    rte_eal_remote_launch(lcore_main, NULL, worker_lcore);
    rte_eal_mp_wait_lcore();

    print_stats();

    RTE_ETH_FOREACH_DEV(port) {
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
    }
    rte_eal_cleanup();
    printf("Encerrado.\n");
    return 0;
}
