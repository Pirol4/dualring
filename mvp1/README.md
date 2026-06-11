# MVP 1 — dual_ring_fwd

Encaminhador L2 com anéis duplos de recepção (fast/burst) sobre DPDK.
Base de comparação: mesmo trabalho por pacote do l2fwd (swap de MACs).

## Arquitetura

- **Fast pool**: `rte_mempool` pequeno (padrão 2048 mbufs ≈ 4,4 MiB — cabe num
  CCX de 16 MiB do EPYC 7402P). Alimenta a fila de RX da NIC; reciclagem rápida
  mantém o working set quente na LLC (DDIO efetivo).
- **Burst pool + ring**: mempool grande em DRAM + `rte_ring` de trabalho
  diferido. Sob pressão no fast pool (rajada), os pacotes são copiados para o
  burst pool e os buffers fast liberados imediatamente — mitigação do leaky DMA.
- **MVP 1 = estático**: gatilho de spill por watermark fixo
  (`--spill-watermark`). O dinamismo via LLC-load-misses entra no MVP 2/3.

## Build

Local (DPDK do sistema):
```bash
make
```

CloudLab (DPDK compilado da fonte em ~/dpdk):
```bash
export PKG_CONFIG_PATH=$(find ~/dpdk/build -name 'libdpdk.pc' -exec dirname {} \; | head -1)
make
```

## Execução no CloudLab (DUT)

```bash
sudo ./dual_ring_fwd -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -T 1
# Flags úteis:
#   --fast-mbufs 2048      tamanho do fast pool
#   --burst-mbufs 65536    tamanho do burst pool
#   --spill-watermark 256  gatilho do spill
#   --force-spill          força o caminho de spill (debug)
```

## Testes locais (sem hardware, vdev pcap)

```bash
# Gerar pcaps: ver gen_test_pcap.py
python3 gen_test_pcap.py

# Caminho rápido
timeout -s INT 8 ./dual_ring_fwd --no-huge -m 700 -l 0 \
    --vdev "net_pcap0,rx_pcap=in.pcap,tx_pcap=out0.pcap" \
    --vdev "net_pcap1,rx_pcap=empty.pcap,tx_pcap=out1.pcap" -- 

# Caminho de spill
... -- --force-spill
```

## Status de validação (11/06/2026, DPDK 23.11 local)

| Item | Status |
|---|---|
| Compilação sem warnings (`-Wall -Wextra`) | OK |
| Caminho rápido: 5000 pkts, 0 perdas, pool devolvido | OK |
| Caminho de spill (forçado): 5000 spilled → 5000 drenados, 0 perdas | OK |
| MAC swap + integridade do payload (verificado no pcap de saída) | OK |
| Spill por pressão REAL no pool | **PENDENTE — só testável em NIC real** |

### Por que o spill por pressão não é testável com vdev pcap

O PMD pcap aloca mbufs sob demanda e os devolve após o TX — o pool nunca fica
pressionado. Numa NIC real, o anel de RX retém `nb_rxd` (1024) descritores
permanentemente preenchidos: com `--fast-mbufs 1100`, sobram ~76 livres
(< watermark 256) e o spill ativa por pressão genuína.

**Primeiro teste no CloudLab:** rodar com `--fast-mbufs 1280 -T 1` sob tráfego
steady e confirmar `spilled > 0` nas estatísticas; depois dimensionar o
watermark com tráfego bursty.

## Pendências conhecidas (decisões conscientes do MVP)

- Single core (datapath no core principal) — multicore é trabalho futuro
- `rte_mempool_avail_count()` a cada iteração: custo aceitável no MVP;
  otimizar amostragem se aparecer no perfil
- A cópia no spill (`rte_pktmbuf_copy`) é deliberada: é o que libera o buffer
  fast imediatamente. Custo da cópia vs. perda por leaky DMA = trade-off
  central a medir no TCC.
