#!/bin/bash
# scripts/run_server.sh — Inicia a implementação DPDK no servidor DUT (TCC DualRing)
#
# Uso:
#   sudo ./scripts/run_server.sh <impl> [duration_s]
#
#   impl (as DUAS configurações da avaliação principal):
#     privring   — método clássico: anel único pequeno, sem transbordo
#                  (l2fwd_dr --disable-burst). Encaminha tudo inline.
#     dualring   — DualRing: anel rápido pequeno + transbordo em DRAM.
#
#   impl (variantes opcionais p/ análise complementar):
#     privring_big — privRing com anel grande (1024): absorve rajada, MAS
#                    sofre o Leaky DMA (outliers de latência, ver relatório).
#     l2fwd        — exemplo l2fwd padrão do DPDK (referência externa).
#
#   duration_s: segundos mantendo o app vivo (default: 750).
#
# COMPARAÇÃO JUSTA (privring vs dualring)
#   Ambos usam o MESMO binário (l2fwd_dr), o MESMO anel rápido pequeno
#   (--rx-ring-size 128) e a MESMA carga por pacote (--work-per-pkt/--work-mem-mb).
#   A ÚNICA diferença é o mecanismo de transbordo. Assim, qualquer diferença de
#   perda/latência é atribuível ao DualRing, e não a configuração de anel/PMD.
#
#   Por que o anel pequeno em AMBOS: o relatório já mostra que um anel grande
#   evita descarte mas POLUI a LLC (4049 outliers ≥1 ms). O privring pequeno
#   isola o efeito do transbordo; o privring_big (opcional) mostra o outro chifre
#   do trade-off. O DualRing vence os dois: baixa perda E baixa cauda.
#
# Fluxo:
#   Terminal 1 (DUT): sudo ./scripts/run_server.sh dualring 750 | tee /tmp/server.log
#   Terminal 2 (DUT): sudo ./scripts/collect_server.sh dualring $EXP_ID 750
#   Servidor T-Rex:   ./collect_client.sh dualring $EXP_ID
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

IMPL="${1:?Uso: sudo $0 <privring|dualring|privring_big|l2fwd> [duration_s]}"
DURATION="${2:-750}"

NIC0="0000:41:00.0"
NIC1="0000:41:00.1"
EAL_ARGS="-l 0-1 -n 4 -a ${NIC0} -a ${NIC1}"
APP_ARGS="-p 0x3 -T 2"   # -T 2: stats a cada 2s

DPDK_BUILD="$HOME/dpdk/build"
DR_DIR="$(cd "$(dirname "$0")/.." && pwd)/src"

# ── Parâmetros DualRing (calibrados para 2 lcores / 2 portas) ────────────────
RX_RING=128          # anel RX pequeno, residente em LLC (fast path)
RX_RING_BIG=1024     # anel grande p/ a variante opcional privring_big
FAST_MBUFS=2048      # fast pool (~4.4 MiB) — cabe na fatia de LLC do CCX
BURST_MBUFS=65536    # burst pool em DRAM (absorve a rajada)
RX_BURST_WM=64       # spill quando a fila RX tem ≥ 64 descritores ocupados
SPILL_WM=256         # guarda de fome de mbufs (secundário)

# ── WorkPackage (réplica do ShRing/FastClick) ───────────────────────────────
# Forçar LLC miss é o que torna o forward CARO e o spill BARATO — condição
# para o DualRing reduzir perda. APLICADO EM AMBOS privring e dualring.
WORK_MEM=80          # 80 MiB > capacidade DDIO → LLC miss garantido
WORK_PKT=4           # 4 acessos aleatórios por pacote
WORK_ARGS="--work-per-pkt ${WORK_PKT} --work-mem-mb ${WORK_MEM}"

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Limpa lock DPDK de runs anteriores
sudo rm -rf /var/run/dpdk/rte/ 2>/dev/null || true

case "${IMPL}" in

  privring)
    log "privRing (método clássico): anel único ${RX_RING}, sem transbordo, com WorkPackage"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING} --fast-mbufs ${FAST_MBUFS} \
      --disable-burst ${WORK_ARGS}"
    ;;

  dualring)
    log "DualRing: anel rápido ${RX_RING} + transbordo DRAM (${BURST_MBUFS}), com WorkPackage"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING} --fast-mbufs ${FAST_MBUFS} \
      --burst-mbufs ${BURST_MBUFS} --rx-burst-watermark ${RX_BURST_WM} \
      --spill-watermark ${SPILL_WM} ${WORK_ARGS}"
    ;;

  privring_big)
    log "privRing anel GRANDE ${RX_RING_BIG} (opcional): absorve rajada, sofre Leaky DMA"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING_BIG} --fast-mbufs 8192 \
      --disable-burst ${WORK_ARGS}"
    ;;

  l2fwd)
    log "l2fwd padrão do DPDK (referência externa, sem WorkPackage)"
    CMD="${DPDK_BUILD}/examples/dpdk-l2fwd ${EAL_ARGS} -- ${APP_ARGS}"
    ;;

  *)
    echo "Implementação desconhecida: ${IMPL}"
    echo "Opções: privring | dualring | privring_big | l2fwd"
    exit 1
    ;;
esac

log "Comando: ${CMD}"
log "Duração esperada: ~${DURATION}s (até collect_client.sh terminar)"
log ""

timeout --kill-after=10 "${DURATION}" bash -c "sudo ${CMD}" || true

log "Implementação encerrada."
