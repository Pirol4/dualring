#!/bin/bash
# scripts/setup_shring.sh — Compila o ShRing (BorisPis/shRing-dpdk, fork DPDK 21.05)
#
# TIMEBOX: se este script não terminar com sucesso em 90 minutos, abandone ShRing.
# A comparação qualitativa via paper (OSDI 2023) é uma alternativa válida para o TCC.
#
# Uso: ./scripts/setup_shring.sh
# (não precisa de sudo; o build do DPDK não precisa de root)
#
# Requisito: dependências de build já instaladas pelo setup.sh (meson, ninja,
# python3-pyelftools, libssl-dev, etc.)
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

log() { echo "[$(date +%H:%M:%S)] $*"; }
die() { log "ERRO: $*"; exit 1; }

INSTALL_DIR="$HOME/shring-dpdk"
BUILD_DIR="${INSTALL_DIR}/build"
REPO_URL="https://github.com/BorisPis/shRing-dpdk.git"

# ── 1. Clone ──────────────────────────────────────────────────────────────────
if [[ -d "${INSTALL_DIR}/.git" ]]; then
    log "shRing-dpdk já clonado em ${INSTALL_DIR}"
else
    log "Clonando ${REPO_URL}..."
    git clone --depth=1 "${REPO_URL}" "${INSTALL_DIR}" \
        || die "git clone falhou"
fi

cd "${INSTALL_DIR}"

log "Branch/commit atual:"
git log --oneline -3

# ── 2. Dependências adicionais (mlx5 PMD) ────────────────────────────────────
log "Verificando dependências mlx5..."
dpkg -l libibverbs-dev ibverbs-providers 2>/dev/null | grep '^ii' | awk '{print $2, $3}' || true

missing=""
for pkg in libibverbs-dev ibverbs-providers; do
    dpkg -l "${pkg}" 2>/dev/null | grep -q '^ii' || missing="${missing} ${pkg}"
done
if [[ -n "${missing}" ]]; then
    log "Instalando dependências faltantes:${missing}"
    sudo apt-get install -y ${missing}
fi

# ── 3. Configure + Build ──────────────────────────────────────────────────────
log "Configurando build (meson)..."
meson setup "${BUILD_DIR}" \
    --prefix="${INSTALL_DIR}/install" \
    -Dexamples=l2fwd \
    -Dplatform=native \
    -Dmachine=native \
    || die "meson setup falhou"

log "Compilando ShRing (ninja)... isto pode levar 30-60 minutos"
time sudo ninja -C "${BUILD_DIR}"

# ── 4. Verifica binário ───────────────────────────────────────────────────────
L2FWD_BIN="${BUILD_DIR}/examples/dpdk-l2fwd"
if [[ -x "${L2FWD_BIN}" ]]; then
    log "✓ ShRing compilado com sucesso: ${L2FWD_BIN}"
    ls -lh "${L2FWD_BIN}"
else
    die "Binário não encontrado: ${L2FWD_BIN}"
fi

# ── 5. Verifica parâmetro --shring ────────────────────────────────────────────
log "Verificando suporte a --shring..."
if sudo "${L2FWD_BIN}" --help -- --help 2>&1 | grep -q shring; then
    log "✓ Flag --shring encontrada"
else
    log "AVISO: --shring não encontrado no help. Verifique se o fork está correto."
    log "       Pode ser que o parâmetro se chame diferente (ex: --shared-ring)"
    sudo "${L2FWD_BIN}" --help -- --help 2>&1 | tail -20 || true
fi

log "════════════════════════════════════════════════════════════"
log "ShRing pronto. Para rodar:"
log ""
log "  sudo rm -rf /var/run/dpdk/rte/"
log "  sudo ${L2FWD_BIN} \\"
log "      -l 0-1 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 \\"
log "      -- -p 0x3 -T 2 --shring 2"
log ""
log "Se falhar, use: scripts/run_server.sh shring"
log "════════════════════════════════════════════════════════════"
