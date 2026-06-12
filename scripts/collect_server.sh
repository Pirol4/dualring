#!/bin/bash
# scripts/collect_server.sh — Coleta LLC stats no DUT durante um experimento
#
# Uso: sudo ./collect_server.sh <implementacao> [exp_id] [duracao_s]
#
#   <implementacao>  Nome da impl: l2fwd | dual_ring_fwd | shring
#   [exp_id]         ID do experimento — use o MESMO do collect_client.sh
#   [duracao_s]      Duração do monitoramento em segundos (padrão: 800)
#                    Cobre 2 perfis × 5 reps × (60s + 10s) = 700s + margem
#
# Fluxo correto:
#   1. Suba a implementação (l2fwd, dual_ring_fwd, etc.) em outro terminal
#   2. Defina o exp_id: exp_id=$(date +%Y%m%d_%H%M%S)
#   3. Neste nó:    sudo ./collect_server.sh <impl> $exp_id
#   4. Client:      ./collect_client.sh <impl> $exp_id
#   5. Aguarde terminar nos dois lados
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

IMPL="${1:-}"
[[ -n "${IMPL}" ]] || { echo "Uso: sudo $0 <implementacao> [exp_id] [duracao_s]"; exit 1; }
EXP_ID="${2:-$(date +%Y%m%d_%H%M%S)}"
DURATION="${3:-800}"

# Resolve o home do usuário real (SUDO_USER) — sem isso, sudo coloca tudo em /root/results/
REAL_USER="${SUDO_USER:-$USER}"
REAL_HOME="$(getent passwd "${REAL_USER}" | cut -d: -f6)"

OUTDIR="${REAL_HOME}/results/${IMPL}/${EXP_ID}"
mkdir -p "${OUTDIR}"
chown "${REAL_USER}:$(id -gn "${REAL_USER}" 2>/dev/null || echo "${REAL_USER}")" "${OUTDIR}"

LLC_OUT="${OUTDIR}/llc_stats.txt"
PERF_INTERVAL_MS=5000   # amostragem a cada 5s

log() { echo "[$(date +%H:%M:%S)] $*"; }

log "════════════════════════════════════════"
log "Implementação : ${IMPL}"
log "Exp ID        : ${EXP_ID}"
log "Duração       : ${DURATION}s"
log "Saída         : ${LLC_OUT}"
log "════════════════════════════════════════"
log ""
log ">>> INICIE o collect_client.sh no gerador AGORA <<<"
log ""

# Salva metadados do experimento
cat > "${OUTDIR}/metadata.txt" << META
impl=${IMPL}
exp_id=${EXP_ID}
start=$(date -Iseconds)
duration_s=${DURATION}
perf_interval_ms=${PERF_INTERVAL_MS}
kernel=$(uname -r)
cpu=$(grep 'model name' /proc/cpuinfo | head -1 | cut -d: -f2 | xargs)
cores_monitored=1-15
META

# Coleta LLC stats com perf stat em intervalos regulares
# -C 1-15 : cores isolados onde roda o datapath DPDK
# -I 5000 : amostra a cada 5 segundos (imprime timestamp + contadores)
perf stat \
    -e LLC-load-misses,LLC-loads,cache-misses,cache-references \
    -C 1-15 \
    -I "${PERF_INTERVAL_MS}" \
    -- sleep "${DURATION}" \
    2>&1 | tee "${LLC_OUT}"

log ""
log "LLC stats salvas em: ${LLC_OUT}"

# Processa o arquivo perf e gera CSV para análise
log "Gerando llc_summary.csv..."

python3 << PYEOF
import re, csv, os

infile  = "${LLC_OUT}"
outfile = "${OUTDIR}/llc_summary.csv"

# Formato perf stat -I: "     5.003187247         1,234,567      LLC-load-misses"
pattern = re.compile(
    r'^\s*([\d.]+)\s+[\d,]+\s+([\d,]+)\s+(\S+)'
)

samples = {}
with open(infile) as f:
    for line in f:
        m = pattern.match(line)
        if not m:
            continue
        ts = float(m.group(1))
        val = int(m.group(2).replace(',', ''))
        event = m.group(3)
        samples.setdefault(ts, {})[event] = val

rows = []
for ts in sorted(samples):
    s = samples[ts]
    llc_misses = s.get('LLC-load-misses', 0)
    llc_loads  = s.get('LLC-loads', 0)
    miss_rate  = round(llc_misses / llc_loads, 6) if llc_loads > 0 else ""
    rows.append({
        "timestamp_s"   : ts,
        "llc_loads"     : llc_loads,
        "llc_misses"    : llc_misses,
        "miss_rate"     : miss_rate,
        "cache_misses"  : s.get('cache-misses', ""),
        "cache_refs"    : s.get('cache-references', ""),
    })

if rows:
    with open(outfile, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    print(f"llc_summary.csv: {len(rows)} amostras de {PERF_INTERVAL_MS/1000:.0f}s")
    # Resumo rápido
    rates = [r["miss_rate"] for r in rows if r["miss_rate"] != ""]
    if rates:
        avg = sum(rates) / len(rates)
        print(f"LLC miss rate médio: {avg:.4f} ({avg*100:.2f}%)")
else:
    print("Nenhuma amostra perf encontrada no arquivo.")
PYEOF

log "════════════════════════════════════════"
log "Coleta do servidor concluída: ${OUTDIR}"
log "════════════════════════════════════════"
