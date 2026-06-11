# DualRing — Alocação Dinâmica de Buffers na LLC sobre DPDK

TCC de Mestrado (PPGCC/UFMG) — implementação de um sistema de anéis duplos de
recepção que elimina o trade-off entre absorção de micro-bursts e eficiência de
cache em redes de alta velocidade.

---

## O problema: Leaky DMA

NICs modernas escrevem pacotes diretamente na LLC via Intel DDIO (ou equivalente
AMD). Isso é eficiente quando o anel de RX é pequeno — o working set fica quente
na cache. Mas rajadas de tráfego forçam anéis grandes, que expulsam dados úteis
da LLC. Este é o **Leaky DMA**: anéis grandes absorvem bursts mas "vazam" cache.

A solução do estado da arte (ShRing) usa anéis compartilhados de tamanho fixo.
Este projeto propõe uma alternativa dinâmica, em software puro, sem modificar
firmware ou hardware.

---

## A solução: Dual Receive Ring

Dois mempools DPDK com papéis distintos:

- **Fast pool** — pequeno (~4 MiB), dimensionado para residir num CCX da LLC do
  AMD EPYC 7402P. Alimenta a fila de RX da NIC; reciclagem rápida mantém o
  working set quente (DDIO efetivo).
- **Burst pool** — grande (~140 MiB, DRAM). Sob pressão no fast pool (rajada de
  tráfego), pacotes são copiados para o burst pool e os buffers fast liberados
  imediatamente — mantendo o working set da NIC pequeno mesmo durante bursts.

O monitoramento de `LLC-load-misses` via `perf_event_open` guia a redistribuição
dinâmica entre os dois pools (MVP 2).

---

## Estrutura do repositório

```
mvp1/
  dual_ring_fwd.c     encaminhador L2 com dual rings (contribuição central)
  Makefile
  gen_test_pcap.py    gera pcaps para teste local sem hardware
  README.md           instruções de build, execução e validação

setup.sh              setup DPDK + hugepages nos nós CloudLab (ambos)
setup_trex.sh         instala e configura T-Rex no nó gerador
profile.py            topologia CloudLab: 2× d6515 + 2 enlaces 100G

results/              dados experimentais (CSV por configuração)
```

---

## Ambiente

Dois nós **AMD EPYC d6515** no CloudLab Utah, conectados back-to-back por
2× NICs NVIDIA/Mellanox ConnectX-5 Ex de 100 GbE.

| Nó | Papel |
|---|---|
| Servidor 1 (DUT) | Roda o dual_ring_fwd |
| Servidor 2 (client) | Gerador de carga T-Rex v3.06 |

Software: Ubuntu 20.04, DPDK 22.11 LTS, kernel 5.4.0.

---

## Rodando o MVP 1

### 1. Setup dos nós (ambos)

```bash
sudo bash setup.sh
sudo reboot
```

### 2. Setup do gerador (Servidor 2)

```bash
sudo bash setup_trex.sh
```

### 3. Baseline l2fwd (Servidor 1)

```bash
sudo ~/dpdk/build/examples/dpdk-l2fwd \
    -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1
```

### 4. Compilar e rodar o dual_ring_fwd (Servidor 1)

```bash
cd mvp1/
export PKG_CONFIG_PATH=$(find ~/dpdk/build -name 'libdpdk.pc' -exec dirname {} \; | head -1)
make
sudo ./dual_ring_fwd -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -T 1
```

Flags úteis:

```
--fast-mbufs N        tamanho do fast pool (padrão 2048)
--burst-mbufs N       tamanho do burst pool (padrão 65536)
--spill-watermark N   gatilho de spill: fast avail < N (padrão 256)
--force-spill         força o caminho de spill (debug)
```

### 5. Gerar tráfego (Servidor 2)

```bash
# Tiro curto de validação
run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30

# Coleta de baseline completa (5 repetições)
collect_baseline ~/trex/profiles/bursty_64b.py 60 100% 5
```

### Teste local sem hardware (vdev pcap)

```bash
cd mvp1/
python3 gen_test_pcap.py
# Caminho rápido
timeout -s INT 8 ./dual_ring_fwd --no-huge -m 700 -l 0 \
    --vdev "net_pcap0,rx_pcap=in.pcap,tx_pcap=out0.pcap" \
    --vdev "net_pcap1,rx_pcap=empty.pcap,tx_pcap=out1.pcap" --
```

---

## Sistemas comparados

| Sistema | Descrição |
|---|---|
| **l2fwd** (baseline) | DPDK padrão, anel único privado por core |
| **ShRing** | Anel compartilhado de tamanho fixo (estado da arte) |
| **DualRing** (este trabalho) | Fast pool LLC-residente + burst pool DRAM, redistribuição dinâmica |

---

## Métricas coletadas

| Métrica | Ferramenta |
|---|---|
| Throughput (Mpps) e perda | T-Rex |
| LLC-load-misses | `perf_event_open` / `perf stat` |
| Cycles por pacote | Contadores de ciclo embutidos |
| Ocupação do burst ring | Contadores internos do dual_ring_fwd |
