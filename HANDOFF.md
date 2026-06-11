# HANDOFF.md — Estado do projeto em 11/06/2026

> **Para o Claude Code:** leia este arquivo, depois `CLAUDE.md` (contexto técnico
> do projeto) e `PLAN.md` (cronograma de 10 dias, entrega 20/06). Este arquivo
> registra a sessão de debug de 10-11/06 e o estado exato de onde paramos.

## Resumo do projeto (1 parágrafo)

TCC: alocação dinâmica de buffers na LLC com **anéis duplos de recepção** sobre
DPDK, comparando contra (1) DPDK l2fwd baseline e (2) ShRing (estado da arte).
Ambiente: CloudLab, 2 nós AMD EPYC d6515 (EPYC 7402P, LLC 128 MiB em CCXs de
16 MiB), ConnectX-5 100Gb, Ubuntu 20.04. Nó "server" = DUT; nó "client" =
gerador T-Rex v3.06. Modelo AF_XDP (fill/completion) é inspiração de design
apenas; implementação 100% DPDK (rte_ring + 2 rte_mempool). Só RX nesta fase.
Monitoramento LLC via perf_event_open userspace, SEM eBPF.

## Estado atual (11/06)

**Dia 1 (pipeline ponta a ponta) — BLOQUEADO até as 19h.** Motivo: o experimento
CloudLab original foi provisionado com um único enlace de 25G na NIC Broadcom
(bnxt_en), e as ConnectX-5 de 100G ficaram SEM LINK (NO-CARRIER). Decisão:
re-instanciar com profile corrigido (`profile.py` na raiz — pede 2 links de
100G). Máquinas novas disponíveis às 19h de hoje.

**Trabalho adiantado enquanto isso (tudo neste repo):**

| Artefato | Estado |
|---|---|
| `profile.py` | Topologia CloudLab corrigida: 2× d6515 + 2 links 100G. Validado contra a API do geni-lib (Python 2!). |
| `setup_trex.sh` | 5 patches aplicados (ver abaixo). Funções geradoras executadas e artefatos validados localmente. |
| `mvp1/dual_ring_fwd.c` | Esqueleto do MVP 1 COMPILADO (DPDK 23.11 local, alvo 22.11) e datapath testado com vdev pcap: fast path e spill path com 5000 pkts, 0 perdas. |
| `test_env/` | Dockerfile + script p/ testar setups em container Ubuntu 20.04. |
| `PLAN.md` | Cronograma dia a dia até 20/06, com cortes de escopo e gatilhos. |

## Histórico de debug da sessão (10-11/06) — bugs reais encontrados e corrigidos

Cadeia de problemas até o diagnóstico final, em ordem:

1. **`run_trex` falhava com "Python files can not be used with STF mode"** —
   causa raiz: perfis STL (.py) NÃO rodam em modo batch (`-f`/`-d` direto no
   t-rex-64); isso só existe para STF/ASTF. Modelo correto: servidor interativo
   (`t-rex-64 -i`) + cliente via API Python. (O `--port 0 1` como argumento
   também era inválido — flag do console, não do binário.)
2. **`EAL: libmlx5.so.1: version MLX5_1.15 not found`** — o PMD mlx5
   pré-compilado do T-Rex v3.06 exige rdma-core mais nova que a do focal (v28).
   Fix: compilar rdma-core v44 em /usr/local/lib (testado manualmente com
   sucesso na máquina real).
3. **Perfis de tráfego usavam `STLArgParser`** — classe que não existe na API
   STL. E o bursty usava `next='burst'` auto-referente (inválido); o primitivo
   correto é `STLTXMultiBurst`.
4. **`Permission denied` ao editar perfis** — `~/trex` criado como root pelo
   setup. Fix: chown para `$SUDO_USER`.
5. **T-Rex transmitia 0 pps com `link: DOWN`** — diagnóstico final: as
   ConnectX-5 estavam NO-CARRIER porque o experimento não tinha os enlaces de
   100G (ver "Estado atual"). As interfaces com link eram a Broadcom 25G
   (controle + 1 link de experimento em 10.10.1.x).

**Todos os 5 fixes estão codificados no `setup_trex.sh` atual** (funções
`build_rdma_core`, `create_run_helper` reescrito com `run_trex`/`run_trex_stl.py`/
`collect_baseline` novos, perfis regenerados, chown, `check_link_status`).

## O que foi validado vs. o que está pendente

Validado localmente (container):
- Sintaxe e execução das funções geradoras do setup_trex.sh
- Os 3 perfis: import + `get_streams(direction=0, **kwargs)` + tunables do bursty
- Helpers: bash -n, py_compile, dry-run do collect_baseline com mock (JSONL ok)
- profile.py contra o código-fonte do geni-lib (API e compat Python 2)
- mvp1: compilação limpa (-Wall -Wextra) + datapath com pcap vdev (fast/spill)

Pendente (só no hardware real, às 19h):
- Build da rdma-core v44 via script no focal (comandos provados manualmente)
- check_link_status com as NICs novas
- Servidor T-Rex + API + tráfego real ponta a ponta
- **mvp1: spill por pressão REAL no pool** — não testável com vdev pcap (o PMD
  pcap não retém descritores; NIC real segura nb_rxd=1024 buffers). Primeiro
  teste: `--fast-mbufs 1280 -T 1` sob tráfego steady → conferir `spilled > 0`.

## Roteiro das 19h (dia 1 — gate: pipeline fechado)

1. Instanciar experimento novo (profile `dual-ring-100g`, cluster Utah)
2. Checklist de link ANTES de qualquer setup, nos 2 nós:
   `ip -br link` (ConnectX-5 com LOWER_UP), `ethtool <if> | grep Speed`
   (100000Mb/s), `sudo ethtool -i <if> | grep bus-info` (conferir se as PCI
   continuam 0000:41:00.x — se mudarem, ajustar trex_cfg.yaml e os -a do DUT)
3. Nos 2 nós: `sudo bash setup.sh` + reboot
4. No client: `sudo bash setup_trex.sh`
5. No DUT: `sudo ~/dpdk/build/examples/dpdk-l2fwd -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1`
6. No client: `run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30`
7. Sucesso = TX ≈ RX no resultado do run_trex + contadores subindo no l2fwd

## Próximos passos após o gate (ordem do PLAN.md)

- **Dia 2 — baseline:** `collect_baseline` com steady_64b e bursty_64b, 5 reps,
  resultados em `~/results/*/all_runs.jsonl` → commitar CSVs
- **Falta criar: script de parsing** (JSONL → CSV → média/desvio/p99 + gráficos)
- **Dias 3-5 — MVP 1 no hardware:** `make` em mvp1/ no DUT (PKG_CONFIG_PATH do
  DPDK compilado — ver mvp1/README.md), validar spill por pressão, benchmark
- **Dia 6-7 — MVP 2:** monitor LLC via perf_event_open + watermark dinâmico
  com histerese; e timebox de 1 dia para o build do ShRing (fork DPDK 21.05)

## Decisões de design do mvp1 (revisar criticamente antes de evoluir)

- O spill COPIA o pacote (`rte_pktmbuf_copy`) para liberar o buffer fast
  imediatamente — o trade-off custo-de-cópia vs. leaky-DMA é a pergunta de
  pesquisa central, então a cópia é deliberada
- Single core, anéis SP/SC — multicore é trabalho futuro (congelado no PLAN.md)
- `rte_mempool_avail_count()` por iteração — otimizar só se aparecer no perfil
