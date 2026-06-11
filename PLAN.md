# PLAN.md — Plano de desenvolvimento do TCC (10 dias)

**Entrega: 20/06/2026** · Hoje: 10/06/2026 · **10 dias corridos**

## Objetivo

Implementar alocação dinâmica de buffers na LLC usando dual receive rings sobre DPDK
e comparar contra: (1) DPDK l2fwd (baseline) e (2) ShRing (estado da arte).

## Decisões de arquitetura (TRAVADAS — não reabrir)

- **Modelo AF_XDP como inspiração de design apenas.** O modelo de 4 anéis (fill/completion/RX/TX)
  inspira a estrutura, mas a implementação usa **2 anéis de recepção** (RX only nesta fase).
- **Implementação 100% sobre DPDK**: `rte_ring` + dois `rte_mempool`:
  - **Fast ring**: mempool pequeno, dimensionado para residir na LLC do AMD EPYC 7402P
  - **Burst ring**: mempool grande em DRAM, absorve rajadas
- **Monitoramento LLC**: `perf_event_open` direto do userspace (LLC-load-misses).
  **SEM eBPF** — polling direto da syscall é mais simples e suficiente. eBPF vira trabalho futuro.
- **TX path**: fora do escopo. Trabalho futuro.

## Cronograma dia a dia

### Dia 1 — Ter 10/06 · Ambiente fechado
- [ ] Validar `setup.sh` e `setup_trex.sh` (test_env/ via Docker local)
- [ ] Subir os 2 nós no CloudLab (d6515) e rodar os setups
- [ ] Pipeline ponta a ponta funcionando: T-Rex → l2fwd → contadores
- **Gate: se o pipeline não fechar hoje, dia 2 é só isso. Nada avança sem ele.**

### Dia 2 — Qua 11/06 · Baseline
- [ ] l2fwd com perfis steady_64b e bursty_64b (steady_1500b se sobrar tempo)
- [ ] **5 repetições** por configuração (reduzido de 10 — aceitável com desvio padrão reportado)
- [ ] Script de parsing da saída do T-Rex → CSV versionado no git
- Entregável: `results/baseline/*.csv` + gráfico preliminar

### Dias 3–5 — Qui 12 a Sáb 14/06 · MVP 1: dual rings estáticos
- [ ] Estrutura: 2 mempools (fast LLC-sized, burst DRAM) + lógica de RX que tenta fast primeiro
- [ ] Spill: quando fast esgota, aloca do burst
- [ ] Base de código: partir do l2fwd (mesma estrutura de polling) — NÃO escrever do zero
- [ ] Benchmark com os mesmos perfis do baseline
- **Este é o coração do TCC. Se só isso existir no dia 20, o trabalho ainda é defensável:**
  *"particionamento estático fast/burst reduz LLC misses em X% sob tráfego bursty"*

### Dias 6–7 — Dom 15 a Seg 16/06 · MVP 2: dinamismo simplificado
- [ ] Thread de monitoramento lendo LLC-load-misses via `perf_event_open` (intervalo ~100ms)
- [ ] Política simples de limiar (threshold): miss rate alto → desloca alocação para burst;
      miss rate baixo → favorece fast. **Sem algoritmo sofisticado** — um if com histerese basta.
- [ ] Benchmark
- Fallback: se a integração travar, reportar MVP 1 + medições de perf como análise

### Dia 7 (paralelo) — Seg 16/06 · ShRing — TIMEBOX DE 1 DIA
- [ ] Tentar compilar o fork DPDK 21.05 e rodar 1 perfil
- **Se não compilar/rodar em 1 dia: ABANDONAR.** Comparação com ShRing vira qualitativa
  (números do paper) + justificativa honesta na seção de metodologia. Isso é aceitável.

### Dias 8–9 — Ter 17 a Qua 18/06 · Experimentos finais + gráficos
- [ ] Rodada final: todos os sistemas disponíveis, mesmos perfis, mesmas condições, 5 reps
- [ ] Gráficos finais: throughput (Mpps), perda, latência (avg/p99), LLC-load-misses
- [ ] Congelar resultados — nada de re-rodar depois disso

### Dias 9–10 — Qua 18 a Sáb 20/06 · Escrita final
- [ ] Consolidar texto (ver "escrita contínua" abaixo)
- [ ] Resultados + discussão + conclusão
- [ ] Revisão completa e submissão

## Escrita contínua (REGRA INEGOCIÁVEL)

**1h–1h30 por dia, desde hoje**, nas seções que não dependem de resultados:
- Dia 1–2: fundamentação (leaky DMA, LLC, DDIO, AF_XDP como modelo conceitual)
- Dia 3–4: trabalhos relacionados (ShRing em destaque)
- Dia 5–7: metodologia (hardware d6515, T-Rex, perfis, métricas)
- Dia 8–10: arquitetura da solução, resultados, conclusão

## Lista de cortes (já decididos — não renegociar consigo mesmo)

| Cortado | Vira |
|---|---|
| TX path (4 anéis completos) | Trabalho futuro |
| eBPF para monitoramento | `perf_event_open` userspace; eBPF = trabalho futuro |
| Política de realocação sofisticada | Threshold com histerese |
| 10 repetições | 5 repetições + desvio padrão |
| steady_1500b obrigatório | Opcional, só se sobrar tempo |
| ShRing com >1 dia de debug | Comparação qualitativa via paper |
| Multicore / RSS | Single core RX; trabalho futuro |

## Riscos e gatilhos de decisão

1. **Pipeline não fecha no dia 1** → dia 2 inteiro vira debug de ambiente; baseline desliza
   para dia 3 e MVP 1 perde um dia (compensar cortando steady_1500b e ShRing).
2. **MVP 1 não está medindo até o fim do dia 5** → cancelar MVP 2 e ShRing;
   dias 6–7 viram debug + benchmark do MVP 1.
3. **Qualquer experimento contradiz a hipótese** → isso NÃO é um problema; resultado
   negativo bem analisado também é contribuição. Não gastar dias "consertando" números.

## Comandos de referência rápida

```bash
# DUT (servidor 1)
sudo ~/dpdk/build/examples/dpdk-l2fwd -l 0-15 -n 4 \
    -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1

# Gerador (servidor 2)
run_trex -f ~/trex/profiles/steady_64b.py -m 100% -d 60 --port 0 1
collect_baseline ~/trex/profiles/bursty_64b.py 60 100% 5

# Monitor LLC no DUT (validação manual antes do MVP 2)
sudo perf stat -e LLC-load-misses,LLC-loads -C 1-15 -- sleep 10
```
