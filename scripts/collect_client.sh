#!/bin/bash
# scripts/collect_client.sh — Coleta de experimento no gerador (client/T-Rex)
#
# Uso: ./collect_client.sh <implementacao> [exp_id]
#
#   <implementacao>  Nome da impl sendo medida: privring | dualring
#                    (também aceita privring_big | l2fwd p/ análise complementar)
#   [exp_id]         ID opcional do experimento (padrão: YYYYMMDD_HHMMSS)
#                    Use o MESMO exp_id no collect_server.sh para correlacionar.
#
# Exemplo:
#   exp_id=$(date +%Y%m%d_%H%M%S)
#   # Server: sudo ~/POC_DualRingProject/scripts/collect_server.sh dualring $exp_id
#   # Client: ~/POC_DualRingProject/scripts/collect_client.sh dualring $exp_id
#
# PARÂMETROS FIXOS — não altere entre implementações:
#   steady_64b          : 3% de line rate, 60s por rep (sanidade, sem perda)
#   bursty_64b          : multiplicador 1 (saturação 100%), 60s por rep
#   bursty_sustain_64b  : multiplicador 1 (rajada sustentável), 60s por rep
#                         ← é AQUI que o DualRing reduz a perda vs privring
#   Repetições : 5  ·  Intervalo : 10s entre reps
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

IMPL="${1:-}"
[[ -n "${IMPL}" ]] || { echo "Uso: $0 <implementacao> [exp_id]"; exit 1; }
EXP_ID="${2:-$(date +%Y%m%d_%H%M%S)}"

# Parâmetros fixos
STEADY_RATE="3%"
BURSTY_RATE="1"
SUSTAIN_RATE="1"     # rajada sustentável (regime intermediário) — pps vem do perfil
DURATION=60
REPS=5
INTER_REP_SLEEP=10

OUTDIR="$HOME/results/${IMPL}/${EXP_ID}"
PROFILES_DIR="$HOME/trex/profiles"

mkdir -p "${OUTDIR}"

log() { echo "[$(date +%H:%M:%S)] $*"; }

log "════════════════════════════════════════"
log "Implementação : ${IMPL}"
log "Exp ID        : ${EXP_ID}"
log "Saída         : ${OUTDIR}"
log "Params fixos  : steady=${STEADY_RATE} bursty=${BURSTY_RATE} dur=${DURATION}s reps=${REPS}"
log "════════════════════════════════════════"
echo ""

run_profile() {
    local profile_name="$1"
    local rate="$2"
    local profile_path="${PROFILES_DIR}/${profile_name}.py"

    [[ -f "${profile_path}" ]] || { log "ERRO: perfil não encontrado: ${profile_path}"; return 1; }

    log "--- ${profile_name} @ ${rate} ---"

    local failed=0
    for ((i=1; i<=REPS; i++)); do
        log "Rep ${i}/${REPS}..."
        local outfile="${OUTDIR}/${profile_name}_rep${i}.txt"
        local errfile="${OUTDIR}/${profile_name}_rep${i}.stderr"

        if run_trex -f "${profile_path}" -m "${rate}" -d "${DURATION}" \
            > "${outfile}" 2>"${errfile}"; then
            local json_line
            json_line=$(grep '^JSON_STATS:' "${outfile}" | head -1 | sed 's/^JSON_STATS: //' || true)
            if [[ -n "${json_line}" ]]; then
                local loss_pct tx rx
                loss_pct=$(echo "${json_line}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(f'{d[\"loss_pct\"]:.3f}%')" 2>/dev/null || echo "?")
                tx=$(echo "${json_line}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['tx'])" 2>/dev/null || echo "?")
                rx=$(echo "${json_line}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(d['rx'])" 2>/dev/null || echo "?")
                log "  → tx=${tx} rx=${rx} loss=${loss_pct}"
            fi
        else
            log "  ERRO na rep ${i} (ver ${errfile})"
            failed=$((failed + 1))
        fi

        [[ $i -lt $REPS ]] && sleep "${INTER_REP_SLEEP}"
    done

    # Consolida JSONs
    grep -h '^JSON_STATS:' "${OUTDIR}/${profile_name}"_rep*.txt 2>/dev/null \
        | sed 's/^JSON_STATS: //' \
        > "${OUTDIR}/${profile_name}_all.jsonl" || true

    log "Concluído: ${profile_name}_all.jsonl (${failed} falha(s))"
    echo ""
}

run_profile "steady_64b"          "${STEADY_RATE}"
run_profile "bursty_64b"          "${BURSTY_RATE}"
run_profile "bursty_sustain_64b"  "${SUSTAIN_RATE}"

# ─── Summary CSV ─────────────────────────────────────────────────────────────

log "Gerando summary.csv..."

python3 << PYEOF
import json, os, csv

outdir  = "${OUTDIR}"
impl    = "${IMPL}"
exp_id  = "${EXP_ID}"
rows    = []

def p99_from_hist(hist, total):
    if not hist or not total:
        return ""
    target = total * 0.99
    cum = 0
    for k in sorted(int(k) for k in hist):
        cum += hist.get(str(k), 0)
        if cum >= target:
            return k
    return max(int(k) for k in hist)

for profile in ["steady_64b", "bursty_64b", "bursty_sustain_64b"]:
    jsonl = os.path.join(outdir, f"{profile}_all.jsonl")
    if not os.path.exists(jsonl):
        continue
    with open(jsonl) as f:
        for rep, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                d = json.loads(line)
            except Exception as e:
                print(f"Aviso: JSON inválido rep {rep}: {e}")
                continue

            lat_pg = {}
            for k, v in d.get("latency", {}).items():
                if k != "global" and isinstance(v, dict) and "latency" in v:
                    lat_pg = v["latency"]
                    break

            hist = lat_pg.get("histogram", {})
            lat_total = sum(hist.values()) if hist else 0

            rows.append({
                "impl"       : impl,
                "exp_id"     : exp_id,
                "profile"    : profile,
                "rep"        : rep,
                "tx"         : d.get("tx", 0),
                "rx"         : d.get("rx", 0),
                "loss"       : d.get("loss", 0),
                "loss_pct"   : round(d.get("loss_pct", 0), 4),
                "lat_avg_us" : lat_pg.get("average", ""),
                "lat_p99_us" : p99_from_hist(hist, lat_total),
                "lat_max_us" : lat_pg.get("total_max", ""),
                "jitter_us"  : lat_pg.get("jitter", ""),
            })

csv_path = os.path.join(outdir, "summary.csv")
if rows:
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=rows[0].keys())
        w.writeheader()
        w.writerows(rows)
    print(f"summary.csv gerado: {len(rows)} linhas")
    print(f"Caminho: {csv_path}")

    # Imprime tabela rápida
    print("")
    print(f"{'PERFIL':<15} {'REP':>3} {'TX':>12} {'RX':>12} {'LOSS%':>8} {'LAT_AVG':>9} {'LAT_P99':>9}")
    print("-" * 75)
    for r in rows:
        print(f"{r['profile']:<15} {r['rep']:>3} {r['tx']:>12} {r['rx']:>12} "
              f"{r['loss_pct']:>8.4f} {str(r['lat_avg_us']):>9} {str(r['lat_p99_us']):>9}")
else:
    print("Nenhum dado coletado.")
PYEOF

log "════════════════════════════════════════"
log "Coleta concluída. Resultados em: ${OUTDIR}"
log "Para commitar: git -C ~/POC_DualRingProject add results/ && git commit -m 'results: ${IMPL} ${EXP_ID}'"
log "════════════════════════════════════════"
