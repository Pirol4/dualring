#!/bin/bash
# scripts/run_server.sh — Inicia a implementação DPDK no servidor DUT
#
# Uso:
#   sudo ./scripts/run_server.sh <impl> [duration_s]
#
#   impl:
#     l2fwd            — l2fwd padrão DPDK (privRing grande — baseline)
#     l2fwd_dr_priv    — l2fwd_dr emulando privRing grande (--disable-burst)
#     l2fwd_dr_small   — l2fwd_dr emulando small privRing  (--disable-burst, ring pequeno)
#     l2fwd_dr_dual    — DualRing (ring pequeno + burst ring, work sim ativado)
#     l2fwd_dr_dual_nw — DualRing sem work simulation (só ring)
#     shring           — ShRing (DPDK 21.05 fork, deve estar em ~/shring-dpdk/build)
#
#   duration_s:
#     Segundos para manter o app vivo (default: 750 — cobre 5 reps × 2 perfis)
#
# Fluxo esperado:
#   Terminal 1 (DUT):     sudo ./scripts/run_server.sh l2fwd_dr_dual 750 | tee /tmp/server.log
#   Terminal 2 (DUT):     sudo ./scripts/collect_server.sh l2fwd_dr_dual $EXP_ID 750
#   Servidor T-Rex:       ./collect_client.sh l2fwd_dr_dual $EXP_ID
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

IMPL="${1:?Uso: sudo $0 <impl> [duration_s]}"
DURATION="${2:-750}"

NIC0="0000:41:00.0"
NIC1="0000:41:00.1"
EAL_ARGS="-l 0-1 -n 4 -a ${NIC0} -a ${NIC1}"
APP_ARGS="-p 0x3 -T 2"   # -T 2: stats a cada 2s

DPDK_BUILD="$HOME/dpdk/build"
DR_DIR="$(cd "$(dirname "$0")/.." && pwd)/l2fwd_dr"

# Parâmetros do DualRing (calibrados para 2 lcores, 2 portas)
# fast_pool=2048: NIC(2×128=256) + caches(2×128=256) + watermark(128) + margem
FAST_MBUFS=2048
BURST_MBUFS=65536
SPILL_WM=1400        # alta: ~1024 disponível em steady → spill ativa sob carga
RX_RING_DUAL=128     # ring pequeno para o DualRing
RX_RING_PRIV=1024    # ring grande para o privRing baseline

# Work simulation: réplica do WorkPackage do ShRing (FastClick)
# work_mem_mb=80: força LLC misses (80 MiB > capacidade DDIO ~8 MiB)
# work_per_pkt=4:  4 acessos aleatórios por pacote
WORK_MEM=80
WORK_PKT=4

log() { echo "[$(date +%H:%M:%S)] $*"; }

# Limpa lock DPDK de runs anteriores
sudo rm -rf /var/run/dpdk/rte/ 2>/dev/null || true

case "${IMPL}" in

  l2fwd)
    log "Iniciando l2fwd (privRing padrão DPDK — baseline)"
    CMD="${DPDK_BUILD}/examples/dpdk-l2fwd ${EAL_ARGS} -- ${APP_ARGS}"
    ;;

  l2fwd_dr_priv)
    log "Iniciando l2fwd_dr (privRing grande — ring ${RX_RING_PRIV}, --disable-burst)"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING_PRIV} --fast-mbufs 8192 --disable-burst"
    ;;

  l2fwd_dr_small)
    log "Iniciando l2fwd_dr (small privRing — ring ${RX_RING_DUAL}, --disable-burst)"
    log "AVISO: este modo dropa pacotes sob tráfego bursty (comportamento esperado)"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING_DUAL} --fast-mbufs ${FAST_MBUFS} --disable-burst"
    ;;

  l2fwd_dr_dual_nw)
    log "Iniciando l2fwd_dr (DualRing sem work simulation)"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING_DUAL} --fast-mbufs ${FAST_MBUFS} \
      --burst-mbufs ${BURST_MBUFS} --spill-watermark ${SPILL_WM}"
    ;;

  l2fwd_dr_dual)
    log "Iniciando l2fwd_dr (DualRing com work simulation: ${WORK_PKT} acc/pkt em ${WORK_MEM} MiB)"
    CMD="${DR_DIR}/l2fwd_dr ${EAL_ARGS} -- ${APP_ARGS} \
      --rx-ring-size ${RX_RING_DUAL} --fast-mbufs ${FAST_MBUFS} \
      --burst-mbufs ${BURST_MBUFS} --spill-watermark ${SPILL_WM} \
      --work-per-pkt ${WORK_PKT} --work-mem-mb ${WORK_MEM}"
    ;;

  shring)
    SHRING_L2FWD="$HOME/shring-dpdk/build/examples/dpdk-l2fwd"
    if [[ ! -x "${SHRING_L2FWD}" ]]; then
        log "ERRO: ShRing não compilado. Rode scripts/setup_shring.sh primeiro."
        exit 1
    fi
    log "Iniciando ShRing (shRing/2: 2 cores compartilham 1 ring)"
    CMD="${SHRING_L2FWD} ${EAL_ARGS} -- ${APP_ARGS} --shring 2"
    ;;

  *)
    echo "Implementação desconhecida: ${IMPL}"
    echo "Opções: l2fwd | l2fwd_dr_priv | l2fwd_dr_small | l2fwd_dr_dual | l2fwd_dr_dual_nw | shring"
    exit 1
    ;;
esac

log "Comando: ${CMD}"
log "Duração esperada: ~${DURATION}s (até collect_client.sh terminar)"
log ""

# Executa a implementação; mata automaticamente após DURATION segundos
timeout --kill-after=10 "${DURATION}" bash -c "sudo ${CMD}" || true

log "Implementação encerrada."
