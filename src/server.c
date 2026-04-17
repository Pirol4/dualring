#include "common.h"
#include <signal.h>

static volatile bool force_quit = false;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

// Inicializa uma porta ethernet
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    struct rte_eth_conf port_conf = port_conf_default;
    const uint16_t rx_rings = 1, tx_rings = 1;
    uint16_t nb_rxd = RX_RING_SIZE;
    uint16_t nb_txd = TX_RING_SIZE;
    int retval;
    uint16_t q;
    struct rte_eth_dev_info dev_info;

    if (!rte_eth_dev_is_valid_port(port))
        return -1;

    retval = rte_eth_dev_info_get(port, &dev_info);
    if (retval != 0) {
        printf("Error getting device info: %s\n", strerror(-retval));
        return retval;
    }

    retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
    if (retval != 0)
        return retval;

    retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
    if (retval != 0)
        return retval;

    // Setup RX queue
    for (q = 0; q < rx_rings; q++) {
        retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
                rte_eth_dev_socket_id(port), NULL, mbuf_pool);
        if (retval < 0)
            return retval;
    }

    // Setup TX queue
    for (q = 0; q < tx_rings; q++) {
        retval = rte_eth_tx_queue_setup(port, q, nb_txd,
                rte_eth_dev_socket_id(port), NULL);
        if (retval < 0)
            return retval;
    }

    retval = rte_eth_dev_start(port);
    if (retval < 0)
        return retval;

    struct rte_ether_addr addr;
    retval = rte_eth_macaddr_get(port, &addr);
    if (retval != 0)
        return retval;

    printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
           " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
           port, RTE_ETHER_ADDR_BYTES(&addr));

    retval = rte_eth_promiscuous_enable(port);
    if (retval != 0)
        return retval;

    return 0;
}

// Processa e responde pacotes
static void process_packets(struct rte_mbuf **pkts, uint16_t nb_pkts)
{
    for (uint16_t i = 0; i < nb_pkts; i++) {
        struct rte_mbuf *pkt = pkts[i];
        struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
        
        // Verifica se é pacote IP
        if (eth_hdr->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
            rte_pktmbuf_free(pkt);
            continue;
        }

        struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
        
        // Verifica se é UDP
        if (ip_hdr->next_proto_id != IPPROTO_UDP) {
            rte_pktmbuf_free(pkt);
            continue;
        }

        struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
        struct test_payload *payload = (struct test_payload *)(udp_hdr + 1);

        printf("RX: seq=%lu timestamp=%lu\n", payload->seq_num, payload->timestamp);

        // Inverte endereços MAC para responder
        struct rte_ether_addr tmp_mac;
        rte_ether_addr_copy(&eth_hdr->dst_addr, &tmp_mac);
        rte_ether_addr_copy(&eth_hdr->src_addr, &eth_hdr->dst_addr);
        rte_ether_addr_copy(&tmp_mac, &eth_hdr->src_addr);

        // Inverte IPs
        uint32_t tmp_ip = ip_hdr->src_addr;
        ip_hdr->src_addr = ip_hdr->dst_addr;
        ip_hdr->dst_addr = tmp_ip;

        // Inverte portas UDP
        uint16_t tmp_port = udp_hdr->src_port;
        udp_hdr->src_port = udp_hdr->dst_port;
        udp_hdr->dst_port = tmp_port;

        // Atualiza checksums
        ip_hdr->hdr_checksum = 0;
        udp_hdr->dgram_cksum = 0;

        // Marca para TX
        pkts[i] = pkt;
    }
}

int main(int argc, char *argv[])
{
    struct rte_mempool *mbuf_pool;
    uint16_t nb_ports;
    uint16_t portid = PORT_ID;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Inicializa EAL
    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    nb_ports = rte_eth_dev_count_avail();
    if (nb_ports == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports available\n");

    // Cria mempool
    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    // Inicializa porta
    if (port_init(portid, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);

    printf("Server running on port %u. Press Ctrl+C to exit.\n", portid);

    struct rte_mbuf *bufs[MAX_BURST_SIZE];
    uint64_t total_rx = 0;

    // Loop principal
    while (!force_quit) {
        uint16_t nb_rx = rte_eth_rx_burst(portid, 0, bufs, MAX_BURST_SIZE);

        if (nb_rx == 0)
            continue;

        total_rx += nb_rx;
        process_packets(bufs, nb_rx);

        // Envia respostas
        uint16_t nb_tx = rte_eth_tx_burst(portid, 0, bufs, nb_rx);

        // Libera pacotes que não foram enviados
        if (unlikely(nb_tx < nb_rx)) {
            for (uint16_t i = nb_tx; i < nb_rx; i++)
                rte_pktmbuf_free(bufs[i]);
        }
    }

    printf("\nTotal packets received: %lu\n", total_rx);

    // Cleanup
    rte_eth_dev_stop(portid);
    rte_eth_dev_close(portid);
    rte_eal_cleanup();

    return 0;
}