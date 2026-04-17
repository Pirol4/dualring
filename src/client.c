#include "common.h"
#include <signal.h>

static volatile bool force_quit = false;

static void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        printf("\n\nSignal %d received, preparing to exit...\n", signum);
        force_quit = true;
    }
}

// Inicializa porta (mesmo código do servidor)
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool)
{
    // [Mesmo código do servidor - copie aqui]
    // ...omitido por brevidade, é idêntico
    return 0;
}

// Constrói um pacote UDP
static struct rte_mbuf *create_packet(struct rte_mempool *mbuf_pool,
                                      uint64_t seq_num)
{
    struct rte_mbuf *pkt = rte_pktmbuf_alloc(mbuf_pool);
    if (pkt == NULL)
        return NULL;

    // Ethernet header
    struct rte_ether_hdr *eth_hdr = rte_pktmbuf_mtod(pkt, struct rte_ether_hdr *);
    
    struct rte_ether_addr src_mac = {{CLIENT_MAC_0, CLIENT_MAC_1, CLIENT_MAC_2,
                                      CLIENT_MAC_3, CLIENT_MAC_4, CLIENT_MAC_5}};
    struct rte_ether_addr dst_mac = {{SERVER_MAC_0, SERVER_MAC_1, SERVER_MAC_2,
                                      SERVER_MAC_3, SERVER_MAC_4, SERVER_MAC_5}};
    
    rte_ether_addr_copy(&src_mac, &eth_hdr->src_addr);
    rte_ether_addr_copy(&dst_mac, &eth_hdr->dst_addr);
    eth_hdr->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);

    // IP header
    struct rte_ipv4_hdr *ip_hdr = (struct rte_ipv4_hdr *)(eth_hdr + 1);
    memset(ip_hdr, 0, sizeof(*ip_hdr));
    ip_hdr->version_ihl = 0x45; // IPv4, header length 20 bytes
    ip_hdr->total_length = rte_cpu_to_be_16(sizeof(*ip_hdr) + sizeof(struct rte_udp_hdr) 
                                             + sizeof(struct test_payload));
    ip_hdr->time_to_live = 64;
    ip_hdr->next_proto_id = IPPROTO_UDP;
    ip_hdr->src_addr = rte_cpu_to_be_32(0x0A000001); // 10.0.0.1
    ip_hdr->dst_addr = rte_cpu_to_be_32(0x0A000002); // 10.0.0.2

    // UDP header
    struct rte_udp_hdr *udp_hdr = (struct rte_udp_hdr *)(ip_hdr + 1);
    udp_hdr->src_port = rte_cpu_to_be_16(UDP_SRC_PORT);
    udp_hdr->dst_port = rte_cpu_to_be_16(UDP_DST_PORT);
    udp_hdr->dgram_len = rte_cpu_to_be_16(sizeof(*udp_hdr) + sizeof(struct test_payload));
    udp_hdr->dgram_cksum = 0;

    // Payload
    struct test_payload *payload = (struct test_payload *)(udp_hdr + 1);
    payload->seq_num = seq_num;
    payload->timestamp = rte_rdtsc();

    pkt->data_len = sizeof(*eth_hdr) + sizeof(*ip_hdr) + sizeof(*udp_hdr) 
                    + sizeof(*payload);
    pkt->pkt_len = pkt->data_len;

    return pkt;
}

int main(int argc, char *argv[])
{
    struct rte_mempool *mbuf_pool;
    uint16_t portid = PORT_ID;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Error with EAL initialization\n");

    argc -= ret;
    argv += ret;

    if (rte_eth_dev_count_avail() == 0)
        rte_exit(EXIT_FAILURE, "No Ethernet ports available\n");

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS,
        MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());

    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    if (port_init(portid, mbuf_pool) != 0)
        rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n", portid);

    printf("Client running. Sending packets on port %u. Press Ctrl+C to exit.\n", portid);

    uint64_t seq_num = 0;
    uint64_t total_tx = 0;

    while (!force_quit) {
        struct rte_mbuf *pkt = create_packet(mbuf_pool, seq_num++);
        if (pkt == NULL) {
            printf("Failed to allocate packet\n");
            break;
        }

        uint16_t nb_tx = rte_eth_tx_burst(portid, 0, &pkt, 1);
        
        if (nb_tx == 0) {
            rte_pktmbuf_free(pkt);
        } else {
            total_tx++;
            printf("TX: seq=%lu\n", seq_num - 1);
        }

        rte_delay_us(1000); // 1ms entre pacotes
    }

    printf("\nTotal packets sent: %lu\n", total_tx);

    rte_eth_dev_stop(portid);
    rte_eth_dev_close(portid);
    rte_eal_cleanup();

    return 0;
}