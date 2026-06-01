# CLAUDE.md

## O que é esse projeto?

Pesquisa de Conclusão de Curso que implementa um sistema de alocação dinâmica de buffers na LLC (Last Level Cache) usando uma estrutura de anéis duplos de recepção sobre o DPDK baseado no AF_XDP (https://docs.ebpf.io/linux/concepts/af_xdp/) do eBPF. O objetivo é eliminar o trade-off entre absorção de micro-bursts e eficiência de cache em redes de alta velocidade.

O problema central: NICs modernas fazem DMA diretamente na LLC via Intel DDIO (ou equivalente AMD). Anéis de recepção grandes absorvem rajadas mas expulsam dados úteis da LLC (Leaky DMA); anéis pequenos preservam a cache mas causam perda de pacotes em picos de tráfego. Este projeto resolve esse trade-off de forma dinâmica, em software puro, sem modificar firmware ou hardware.

---

## Ambiente de desenvolvimento

Estou usando duas máquinas d6515 do site CloudLab (https://www.cloudlab.us/) para servirem como servidor e cliente, elas possuem sistema operacional Ubuntu 20.04 LTS. (Todas essas configurações podem ser alteradas se for necessário)

Quero comparar a minha implementação com o DPDK padrão e os estados da arte Shring e RxBisect:
- DPDK (padrão): https://github.com/DPDK/dpdk
- Shring: https://github.com/BorisPis/shRing-dpdk
- RxBisect: (Depois vou comparar com ele, começar só pelo Shring)

Baseado no setup do artigo **Shring**, esta é a configuração de referência para o desenvolvimento.

---

## Hardware

### Servidores
- **Modelo:** 2x Dell PowerEdge R640 (conectados back-to-back)
- **CPU:** Intel Xeon Silver 4216 — 16 núcleos, 2,1 GHz
- **Memória RAM:** 128 GiB DDR4 2933 MHz (4x 16 GiB)
- **LLC (Last-Level Cache):** 22 MiB, 11 ways
- **NICs:** 2x pares NVIDIA ConnectX-5 (100 GbE), **pause frames desabilitados**

### Papéis dos servidores
| Servidor | Função |
|---|---|
| Servidor 1 | Sistema avaliado |
| Servidor 2 | Gerador de carga |

---

## Software

- **OS:** Ubuntu 18.04
- **Kernel:** Linux 5.4.0
- **Gerador de pacotes:** Cisco T-Rex (stateless), modificado para precisão de latência de **1 µs** (padrão original: 10–100 µs)

---

## Configurações do Kernel

| Configuração | Valor |
|---|---|
| CPU isolation (OS scheduler) | Ativado |
| Hugepages | 1 GiB |
| Power saving states | Desabilitado |
| Microarchitectural side-channel mitigations | Desabilitado |
| Hyperthreading | Desabilitado |
| Turbo Boost | Desabilitado |

---

## Configurações Padrão da Aplicação

| Parâmetro | Valor padrão |
|---|---|
| Rx ring (descritores) | 1024 |
| Tx ring (descritores) | 1024 |
| DDIO LLC ways | 2 |
| CPU cores utilizados | 16 (todos disponíveis) |
| Cores por NIC | 8 |

---

## Metodologia de Medição

| Métrica | Ferramenta |
|---|---|
| Cycles per packet | Contadores de ciclo embutidos na aplicação |
| Cache hit rate | `Linux perf` |
| Tx ring occupancy | Comparação dos índices producer/consumer do completion ring |
| PCIe latency | NVIDIA Mellanox Neo-host |
| Memory bandwidth e PCIe hit rate | Intel PCM |

---

## Mecanismos de Ring Comparados

| Mecanismo | Descrição |
|---|---|
| **privRing** | Ring privado por core |
| **shRing/8** | Array ring compartilhado (não dinâmico) entre 8 cores (máximo para 16 cores + 2 NICs) |
| **small privRing** | privRing com contagem de descritores equivalente ao shRing/8 (128 entradas/ring); impraticável em tráfego com bursts |

> **Nota:** O small privRing é usado apenas para comparação teórica — ele causa perda de pacotes em tráfego bursty.

---

## Metodologia Estatística

- **Repetições:** 10 execuções por experimento
- **Agregação:** Média aparada (*trimmed mean*) — descartando o mínimo e o máximo
- **Desvio padrão:** Sempre abaixo de 5%
