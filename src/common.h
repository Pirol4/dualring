#ifndef COMMON_H
#define COMMON_H

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>

// Configuração de porta
#define PORT_ID              0
#define NUM_MBUFS            8191
#define MBUF_CACHE_SIZE      250
#define RX_RING_SIZE         1024
#define TX_RING_SIZE         1024
#define MAX_BURST_SIZE       32

// Configuração de pacotes
#define UDP_SRC_PORT         1234
#define UDP_DST_PORT         5678
#define PACKET_SIZE          64

// Endereços MAC (ajuste para suas NICs)
#define SERVER_MAC_0  0x00
#define SERVER_MAC_1  0x11
#define SERVER_MAC_2  0x22
#define SERVER_MAC_3  0x33
#define SERVER_MAC_4  0x44
#define SERVER_MAC_5  0x55

#define CLIENT_MAC_0  0x00
#define CLIENT_MAC_1  0xAA
#define CLIENT_MAC_2  0xBB
#define CLIENT_MAC_3  0xCC
#define CLIENT_MAC_4  0xDD
#define CLIENT_MAC_5  0xEE

// Estrutura de configuração padrão da porta
static const struct rte_eth_conf port_conf_default = {
    .rxmode = {
        .max_lro_pkt_size = RTE_ETHER_MAX_LEN,
    },
};

// Payload do pacote de teste
struct test_payload {
    uint64_t seq_num;
    uint64_t timestamp;
} __rte_packed;

#endif