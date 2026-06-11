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
 * MVP 2/3 = watermark dinâmico guiado por LLC-load-misses (perf_event_open).
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

static unsigned int fast_mbufs       = 2048;   /* tamanho do fast pool        */
static unsigned int burst_mbufs      = 65536;  /* tamanho do burst pool (DRAM)*/
static unsigned int burst_ring_size  = 16384;  /* capacidade do burst ring    */
static unsigned int spill_watermark  = 256;    /* avail mínimo do fast pool   */
static unsigned int stats_period_s   = 0;      /* -T: período de stats (0=off)*/
static bool         force_spill      = false;  /* p/ teste: spill sempre      */

/* ─── Estado global ──────────────────────────────────────────────────────── */

static volatile bool force_quit = false;

struct port_ctx {
    struct rte_mempool *fast_pool;
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
    printf("══════════════════════════════════════════════════════════\n");
}

/* ─── Datapath ───────────────────────────────────────────────────────────── */

/* Swap de MACs origem/destino — mesmo trabalho por pacote do l2fwd,
 * mantendo a comparação justa com o baseline. */
static inline void
mac_swap(struct rte_mbuf *m)
{
    struct rte_ether_hdr *eth =
        rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    struct rte_ether_addr tmp;

    rte_ether_addr_copy(&eth->src_addr, &tmp);
    rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
    rte_ether_addr_copy(&tmp, &eth->dst_addr);
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
        mac_swap(pkts[i]);

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
           "watermark=%u%s)\n",
           rte_lcore_id(), fast_mbufs, burst_mbufs, spill_watermark,
           force_spill ? ", FORCE-SPILL" : "");

    while (!force_quit) {
        RTE_ETH_FOREACH_DEV(port) {
            struct port_ctx *ctx = &g_ctx[port];

            uint16_t nb = rte_eth_rx_burst(port, 0, pkts, BURST_SIZE);
            if (nb > 0) {
                g_stats[port].rx_total += nb;

                /* Decisão fast vs spill: pressão no fast pool?            */
                unsigned int avail =
                    rte_mempool_avail_count(ctx->fast_pool);
                bool under_pressure =
                    force_spill || (avail < spill_watermark);

                if (unlikely(under_pressure)) {
                    spill_pkts(port, pkts, nb);
                } else {
                    for (uint16_t i = 0; i < nb; i++)
                        mac_swap(pkts[i]);
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
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;

    ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0)
        return ret;

    struct port_ctx *ctx = &g_ctx[port];
    ctx->tx_port = (nb_ports >= 2) ? (port ^ 1) : port;

    /* FAST POOL — pequeno, alvo: residência na LLC */
    snprintf(name, sizeof(name), "fast_pool_p%u", port);
    ctx->fast_pool = rte_pktmbuf_pool_create(name, fast_mbufs,
        RTE_MIN(256U, fast_mbufs / 2), 0, RTE_MBUF_DEFAULT_BUF_SIZE,
        rte_eth_dev_socket_id(port));
    if (ctx->fast_pool == NULL)
        rte_exit(EXIT_FAILURE, "Falha ao criar fast pool da porta %u: %s\n",
                 port, rte_strerror(rte_errno));

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
           "  -T N                  imprime stats a cada N segundos\n",
           prog, fast_mbufs, burst_mbufs, burst_ring_size, spill_watermark);
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
        { NULL, 0, NULL, 0 },
    };
    int opt;

    while ((opt = getopt_long(argc, argv, "T:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'f': fast_mbufs      = atoi(optarg); break;
        case 'b': burst_mbufs     = atoi(optarg); break;
        case 'r': burst_ring_size = atoi(optarg); break;
        case 'w': spill_watermark = atoi(optarg); break;
        case 'F': force_spill     = true;         break;
        case 'T': stats_period_s  = atoi(optarg); break;
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

    uint16_t port;
    RTE_ETH_FOREACH_DEV(port) {
        if (port_init(port, nb_ports) != 0)
            rte_exit(EXIT_FAILURE, "Falha ao inicializar porta %u\n", port);
    }

    /* MVP single core: datapath no core principal */
    lcore_main(NULL);

    print_stats();

    RTE_ETH_FOREACH_DEV(port) {
        rte_eth_dev_stop(port);
        rte_eth_dev_close(port);
    }
    rte_eal_cleanup();
    printf("Encerrado.\n");
    return 0;
}
