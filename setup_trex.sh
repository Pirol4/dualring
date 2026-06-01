#!/bin/bash
# setup_trex.sh — Instala e configura T-Rex no Servidor 2 (gerador de carga)
#
# Pré-requisito: sudo ./setup_dpdk.sh deve ter sido executado antes (hugepages, vfio-pci)
#
# Uso:
#   sudo ./setup_trex.sh [--trex-version VER] [--trex-dir DIR] [--port0 BDF] [--port1 BDF]
#
# Flags:
#   --trex-version VER  Versão do T-Rex a baixar (padrão: 3.06)
#   --trex-dir DIR      Diretório de instalação (padrão: ~/trex)
#   --port0 BDF         Endereço PCI da porta 0 (ex: 0000:41:00.0) — detectado auto se omitido
#   --port1 BDF         Endereço PCI da porta 1 (ex: 0000:41:00.1) — detectado auto se omitido

set -euo pipefail

# ─── Configurações ────────────────────────────────────────────────────────────

TREX_VERSION="3.06"
TREX_DIR="${HOME}/trex"
PORT0=""
PORT1=""

# ─── Parse de argumentos ──────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trex-version) TREX_VERSION="$2"; shift ;;
        --trex-dir)     TREX_DIR="$2";     shift ;;
        --port0)        PORT0="$2";        shift ;;
        --port1)        PORT1="$2";        shift ;;
        *) echo "Argumento desconhecido: $1" >&2; exit 1 ;;
    esac
    shift
done

# ─── Helpers ──────────────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()  { echo -e "${GREEN}[$(date +%H:%M:%S)]${NC} $*"; }
warn() { echo -e "${YELLOW}[AVISO]${NC} $*"; }
die()  { echo -e "${RED}[ERRO]${NC} $*" >&2; exit 1; }
sep()  { echo -e "\n${GREEN}═══════════════════════════════════════${NC}"; log "$*"; }

[[ $EUID -eq 0 ]] || die "Execute como root: sudo $0 $*"

TREX_DOWNLOAD_URL="https://trex-tgn.cisco.com/trex/release/v${TREX_VERSION}.tar.gz"
PROFILES_DIR="${TREX_DIR}/profiles"

# ─── 1. Verificação de pré-requisitos ─────────────────────────────────────────

check_prerequisites() {
    sep "Verificando pré-requisitos"

    # Hugepages de 1 GiB (obrigatório para o T-Rex)
    local hp_total
    hp_total=$(grep -i 'HugePages_Total' /proc/meminfo | awk '{print $2}')
    if [[ "${hp_total}" -lt 2 ]]; then
        die "Hugepages insuficientes (${hp_total} disponíveis). Execute setup_dpdk.sh primeiro."
    fi
    log "Hugepages OK: ${hp_total} × 1 GiB disponíveis."

    # Python 3 (necessário para scripts de controle do T-Rex)
    python3 --version &>/dev/null || die "Python 3 não encontrado. Execute setup_dpdk.sh primeiro."
    log "Python 3 OK: $(python3 --version)"

    # scapy (necessário para os perfis de tráfego)
    if ! python3 -c "import scapy" &>/dev/null; then
        log "Instalando scapy..."
        pip3 install scapy --quiet
    fi
    log "scapy OK."

    # vfio-pci (necessário para bind das NICs)
    if ! lsmod | grep -q vfio_pci; then
        log "Carregando módulo vfio-pci..."
        modprobe vfio-pci || warn "vfio-pci não disponível — T-Rex usará uio_pci_generic como fallback."
        modprobe uio_pci_generic || true
    fi
    log "Módulos de kernel OK."

    # rdma-core / libibverbs — necessário para PMD mlx5 do T-Rex sem OFED completo
    if ! dpkg -l rdma-core &>/dev/null; then
        log "Instalando rdma-core (libibverbs para mlx5 PMD)..."
        apt-get install -y --quiet rdma-core libibverbs-dev libmlx5-dev ibverbs-utils 2>/dev/null || \
            warn "Pacotes rdma-core não instalados — mlx5 PMD pode falhar."
    fi
    log "rdma-core OK."
}

# ─── 2. Download e extração do T-Rex ─────────────────────────────────────────

install_trex() {
    sep "Baixando T-Rex v${TREX_VERSION}"

    if [[ -d "${TREX_DIR}/v${TREX_VERSION}" ]]; then
        warn "T-Rex v${TREX_VERSION} já encontrado em ${TREX_DIR}/v${TREX_VERSION} — pulando download."
        return
    fi

    mkdir -p "${TREX_DIR}"
    cd "${TREX_DIR}"

    log "Download de: ${TREX_DOWNLOAD_URL}"
    wget --progress=bar:force --no-check-certificate -O "v${TREX_VERSION}.tar.gz" "${TREX_DOWNLOAD_URL}" || \
        die "Falha no download. Verifique a versão em: https://trex-tgn.cisco.com/trex/release/"

    log "Extraindo..."
    tar -xzf "v${TREX_VERSION}.tar.gz"
    rm -f "v${TREX_VERSION}.tar.gz"

    log "T-Rex instalado em ${TREX_DIR}/v${TREX_VERSION}"
}

# ─── 2b. Compatibilidade Python 3.8+ ─────────────────────────────────────────
# platform.dist() e platform.linux_distribution() foram removidos no Python 3.8.
# Ubuntu 20.04 usa Python 3.8.x, então precisamos corrigir os scripts do T-Rex.

fix_python38_compat() {
    sep "Corrigindo compatibilidade Python 3.8+"

    local trex_install="${TREX_DIR}/v${TREX_VERSION}"

    # Encontra todos os .py que chamam platform.dist ou platform.linux_distribution
    local broken_files
    broken_files=$(grep -rl "platform\.dist\(\|platform\.linux_distribution(" \
        "${trex_install}" --include="*.py" 2>/dev/null || true)

    if [[ -z "${broken_files}" ]]; then
        log "Nenhum arquivo com platform.dist encontrado — OK."
    else
        log "Arquivos para patching:"
        while IFS= read -r f; do
            echo "  ${f}"
            # Injeta shim no topo do arquivo (após a primeira linha shebang/comentário, se existir)
            if ! grep -q "platform\.dist = lambda" "${f}"; then
                sed -i '1s/^/import platform\nif not hasattr(platform, "dist"):\n    platform.dist = lambda *a, **k: ("", "", "")\n    platform.linux_distribution = lambda *a, **k: ("", "", "")\n/' "${f}"
            fi
        done <<< "${broken_files}"
        log "Shim de compatibilidade injetado."
    fi

    # Garante que ofed_info existe (T-Rex verifica presença; sem OFED completo, cria stub)
    if ! command -v ofed_info &>/dev/null; then
        log "Criando stub /usr/bin/ofed_info para passar verificação do T-Rex..."
        cat > /usr/bin/ofed_info << 'STUB'
#!/bin/bash
# Stub: MLNX OFED não instalado; T-Rex usa rdma-core (in-kernel mlx5).
echo "MLNX_OFED_LINUX-5.4-0 (User space)"
STUB
        chmod +x /usr/bin/ofed_info
        log "Stub /usr/bin/ofed_info criado."
    else
        log "ofed_info já presente — OK."
    fi
}

# ─── 3. Detecção automática de NICs ConnectX-5 ────────────────────────────────

detect_nics() {
    sep "Detectando NICs ConnectX-5"

    # Procura dispositivos Mellanox/NVIDIA ConnectX na lista PCI
    local nics
    nics=$(lspci -D | grep -iE 'Mellanox|ConnectX' | grep -v 'Virtual' | awk '{print $1}' || true)

    if [[ -z "${nics}" ]]; then
        die "Nenhuma NIC Mellanox/ConnectX encontrada via lspci. Verifique se os drivers estão carregados."
    fi

    local nic_array=()
    while IFS= read -r nic; do
        nic_array+=("$nic")
    done <<< "${nics}"

    log "NICs encontradas:"
    for nic in "${nic_array[@]}"; do
        echo "  ${nic}  $(lspci -D -s "${nic}" | cut -d: -f4-)"
    done

    if [[ -z "${PORT0}" ]]; then
        PORT0="${nic_array[0]:-}"
        [[ -n "${PORT0}" ]] || die "Não foi possível detectar PORT0. Use --port0 <BDF>."
        log "PORT0 detectada automaticamente: ${PORT0}"
    fi

    if [[ -z "${PORT1}" ]]; then
        PORT1="${nic_array[1]:-}"
        [[ -n "${PORT1}" ]] || die "Não foi possível detectar PORT1. Use --port1 <BDF>."
        log "PORT1 detectada automaticamente: ${PORT1}"
    fi
}

# ─── 4. Bind das NICs para o T-Rex ────────────────────────────────────────────

bind_nics() {
    sep "Configurando NICs para uso com T-Rex"

    # ConnectX-5 suporta modo bifurcado (NIC permanece no driver mlx5_core do kernel
    # E o DPDK acessa via PMD mlx5). T-Rex detecta isso automaticamente — não precisa bind.
    # Se mlx5_core não estiver disponível, faz bind para vfio-pci.

    for port in "${PORT0}" "${PORT1}"; do
        local current_driver
        current_driver=$(lspci -D -k -s "${port}" 2>/dev/null | grep 'Kernel driver in use' | awk '{print $NF}' || echo "none")

        if [[ "${current_driver}" == "mlx5_core" ]]; then
            log "NIC ${port} no driver mlx5_core (modo bifurcado) — T-Rex acessará via PMD mlx5. OK."
        elif [[ "${current_driver}" == "vfio-pci" ]]; then
            log "NIC ${port} já bound para vfio-pci. OK."
        else
            warn "NIC ${port} usando driver '${current_driver}'. Tentando bind para vfio-pci..."
            local devbind
            devbind=$(find /usr/local/share/dpdk -name 'dpdk-devbind.py' 2>/dev/null | head -1)
            devbind=${devbind:-$(which dpdk-devbind.py 2>/dev/null || echo "")}

            if [[ -n "${devbind}" ]]; then
                python3 "${devbind}" --bind=vfio-pci "${port}" || \
                    warn "Falha no bind de ${port} — verifique manualmente."
            else
                warn "dpdk-devbind.py não encontrado. Bind de ${port} deve ser feito manualmente."
            fi
        fi
    done
}

# ─── 5. Geração do /etc/trex_cfg.yaml ────────────────────────────────────────

generate_trex_config() {
    sep "Gerando /etc/trex_cfg.yaml"

    # Detecta número de threads disponíveis (deixa core 0 para o master e core 1 para latência)
    local total_cores
    total_cores=$(nproc --all)
    local worker_start=2
    local worker_end=$((total_cores - 1))

    # Detecta NUMA node das NICs para afinidade de memória
    local numa0
    numa0=$(cat "/sys/bus/pci/devices/${PORT0}/numa_node" 2>/dev/null || echo 0)
    [[ "${numa0}" -lt 0 ]] && numa0=0  # -1 significa sem NUMA info, usa 0

    # Gera lista de threads workers: "2,3,4,...,N"
    local threads=""
    for ((i=worker_start; i<=worker_end; i++)); do
        threads+="${i}"
        [[ $i -lt $worker_end ]] && threads+=","
    done

    cat > /etc/trex_cfg.yaml << EOF
# Configuração T-Rex gerada por setup_trex.sh
# Gerado em: $(date)
# Servidores: servidor 2 (gerador de carga) ↔ servidor 1 (DUT)
# Hardware: ConnectX-5 100 GbE back-to-back

- port_limit      : 2
  version         : 2
  interfaces      : ['${PORT0}', '${PORT1}']

  # IPs de referência para fluxos L3 (podem ser ajustados nos perfis de tráfego)
  port_info       :
    - ip          : 16.0.0.1
      default_gw  : 48.0.0.1
    - ip          : 48.0.0.1
      default_gw  : 16.0.0.1

  platform        :
    master_thread_id  : 0        # Core 0: processo master do T-Rex
    latency_thread_id : 1        # Core 1: medição de latência (precisão ~1 µs)
    dual_if           :
      - socket    : ${numa0}
        threads   : [${threads}]
EOF

    log "Configuração gerada em /etc/trex_cfg.yaml"
    log "  PORT0 (porta 0): ${PORT0}"
    log "  PORT1 (porta 1): ${PORT1}"
    log "  Cores workers  : ${worker_start}-${worker_end} (${total_cores} cores no total)"
}

# ─── 6. Perfis de tráfego ─────────────────────────────────────────────────────

create_traffic_profiles() {
    sep "Criando perfis de tráfego"

    mkdir -p "${PROFILES_DIR}"

    # ── Perfil 1: Fluxo contínuo — pacotes de 64 bytes ──────────────────────────
    cat > "${PROFILES_DIR}/steady_64b.py" << 'EOF'
"""
Perfil: fluxo contínuo de pacotes de 64 bytes
Uso: ./t-rex-64 -f profiles/steady_64b.py -m 100% --port 0 1

Parâmetros ajustáveis via -t:
  rate_pps : taxa em pacotes/s (padrão: None = usa -m do CLI)
"""
from trex_stl_lib.api import *

class STLSteady64(object):

    def get_streams(self, tunables, **kwargs):
        parser = STLArgParser()
        parser.add_argument('--rate-pps', type=float, default=None,
                            help='Taxa explícita em pps (default: controlada pelo -m do CLI)')
        args = parser.parse_args(tunables)

        # Pacote Ethernet/IP/UDP de exatamente 64 bytes (sem FCS)
        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 64 - len(base_pkt) - 4)  # -4 = FCS adicionado pelo hardware
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        # Stream de dados
        if args.rate_pps:
            mode = STLTXCont(pps=args.rate_pps)
        else:
            mode = STLTXCont()

        data_stream = STLStream(
            name='data',
            packet=STLPktBuilder(pkt=pkt),
            mode=mode,
            flow_stats=STLFlowStats(pg_id=1),
        )

        # Stream de latência (1 em cada 1000 pacotes; timestamps de hardware)
        lat_stream = STLStream(
            name='latency',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(pps=1000),       # taxa baixa: não interfere na medição
            flow_stats=STLFlowLatencyStats(pg_id=11),
        )

        return [data_stream, lat_stream]


def register():
    return STLSteady64()
EOF

    # ── Perfil 2: Fluxo contínuo — pacotes de 1500 bytes ────────────────────────
    cat > "${PROFILES_DIR}/steady_1500b.py" << 'EOF'
"""
Perfil: fluxo contínuo de pacotes de 1500 bytes (MTU Ethernet padrão)
Uso: ./t-rex-64 -f profiles/steady_1500b.py -m 100% --port 0 1
"""
from trex_stl_lib.api import *

class STLSteady1500(object):

    def get_streams(self, tunables, **kwargs):
        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 1500 - len(base_pkt) - 4)
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        data_stream = STLStream(
            name='data',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(),
            flow_stats=STLFlowStats(pg_id=2),
        )

        lat_stream = STLStream(
            name='latency',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(pps=1000),
            flow_stats=STLFlowLatencyStats(pg_id=12),
        )

        return [data_stream, lat_stream]


def register():
    return STLSteady1500()
EOF

    # ── Perfil 3: Tráfego bursty — 64 bytes ─────────────────────────────────────
    # Este perfil é o mais importante: revela o trade-off anel grande vs. pequeno.
    # burst_pkts  : número de pacotes por burst (default: 2048 ≈ 2× o anel padrão de 1024)
    # burst_pps   : taxa durante o burst (default: line-rate ~148 Mpps para 64B em 100GbE)
    # idle_us     : pausa entre bursts em microsegundos (default: 10 µs)
    cat > "${PROFILES_DIR}/bursty_64b.py" << 'EOF'
"""
Perfil: tráfego bursty de 64 bytes — revela o trade-off leaky DMA.

Estrutura de cada ciclo:
  burst_pkts pacotes a burst_pps  →  idle_us µs de silêncio  →  repete

Parâmetros ajustáveis via -t:
  --burst-pkts N   Pacotes por burst       (padrão: 2048)
  --burst-pps  N   Taxa durante o burst    (padrão: 100_000_000  = 100 Mpps)
  --idle-us    N   Pausa entre bursts (µs) (padrão: 10)

Uso:
  ./t-rex-64 -f profiles/bursty_64b.py --port 0 1
  ./t-rex-64 -f profiles/bursty_64b.py -t --burst-pkts 4096 --idle-us 50 --port 0 1
"""
from trex_stl_lib.api import *

class STLBursty64(object):

    def get_streams(self, tunables, **kwargs):
        parser = STLArgParser()
        parser.add_argument('--burst-pkts', type=int,   default=2048)
        parser.add_argument('--burst-pps',  type=float, default=100_000_000)
        parser.add_argument('--idle-us',    type=float, default=10)
        args = parser.parse_args(tunables)

        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 64 - len(base_pkt) - 4)
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        # IBG = inter-burst gap em microsegundos → converte para nanossegundos
        ibg_ns = args.idle_us * 1000

        # Stream burst principal: envia burst_pkts a burst_pps, depois pausa ibg_ns
        burst_stream = STLStream(
            name='burst',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXBurst(
                pps=args.burst_pps,
                total_pkts=args.burst_pkts,
            ),
            flow_stats=STLFlowStats(pg_id=3),
            # Após o burst, espera ibg_ns antes do próximo ciclo
            isg=ibg_ns,
            next='burst',           # loop: repete indefinidamente
        )

        # Stream de latência independente (taxa baixa, não interfere no burst)
        lat_stream = STLStream(
            name='latency',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(pps=1000),
            flow_stats=STLFlowLatencyStats(pg_id=13),
        )

        return [burst_stream, lat_stream]


def register():
    return STLBursty64()
EOF

    log "Perfis criados em ${PROFILES_DIR}:"
    log "  steady_64b.py   — fluxo contínuo, 64B"
    log "  steady_1500b.py — fluxo contínuo, 1500B"
    log "  bursty_64b.py   — tráfego bursty, 64B (revela trade-off do anel)"
}

# ─── 7. Script auxiliar de execução ──────────────────────────────────────────

create_run_helper() {
    sep "Criando scripts auxiliares"

    local trex_bin="${TREX_DIR}/v${TREX_VERSION}/t-rex-64"

    # ── run_trex.sh: wrapper geral para iniciar o T-Rex ──────────────────────
    cat > /usr/local/bin/run_trex << EOF
#!/bin/bash
# Wrapper para iniciar o T-Rex em modo stateless interativo.
# Uso: run_trex [args adicionais para t-rex-64]
# Exemplo: run_trex -f ~/trex/profiles/steady_64b.py -m 50% --port 0 1 -d 30

TREX_DIR="${TREX_DIR}/v${TREX_VERSION}"
cd "\${TREX_DIR}"
sudo ./t-rex-64 -i --cfg /etc/trex_cfg.yaml "\$@"
EOF
    chmod +x /usr/local/bin/run_trex

    # ── collect_baseline.sh: coleta as 10 repetições de um experimento ───────
    cat > /usr/local/bin/collect_baseline << 'SCRIPT'
#!/bin/bash
# Executa N repetições de um perfil T-Rex e salva os resultados.
#
# Uso: collect_baseline <perfil> <duracao_s> <taxa> [repeticoes]
#
# Exemplo:
#   collect_baseline ~/trex/profiles/steady_64b.py 60 100% 10
#
# Saída: results/<perfil>_<timestamp>/run_N.json

set -euo pipefail

PROFILE="${1:-}"
DURATION="${2:-60}"
RATE="${3:-100%}"
REPS="${4:-10}"
TREX_DIR="$(ls -d ~/trex/v* 2>/dev/null | sort -V | tail -1)"

[[ -n "${PROFILE}" ]] || { echo "Uso: collect_baseline <perfil> <duracao_s> <taxa> [repeticoes]"; exit 1; }
[[ -f "${PROFILE}" ]] || { echo "Perfil não encontrado: ${PROFILE}"; exit 1; }

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
OUTDIR="$HOME/results/$(basename "${PROFILE%.py}")_${TIMESTAMP}"
mkdir -p "${OUTDIR}"

echo "[$(date +%H:%M:%S)] Iniciando ${REPS} repetições"
echo "  Perfil   : ${PROFILE}"
echo "  Duração  : ${DURATION}s por run"
echo "  Taxa     : ${RATE}"
echo "  Saída    : ${OUTDIR}"
echo ""

for ((i=1; i<=REPS; i++)); do
    echo -n "[$(date +%H:%M:%S)] Run ${i}/${REPS}... "

    cd "${TREX_DIR}"
    # Executa o T-Rex em modo batch (não interativo) com saída JSON
    sudo ./t-rex-64 \
        --cfg /etc/trex_cfg.yaml \
        -f "${PROFILE}" \
        -m "${RATE}" \
        -d "${DURATION}" \
        --port 0 1 \
        --output-file "${OUTDIR}/run_${i}.json" \
        --no-watchdog \
        2>"${OUTDIR}/run_${i}.stderr" || {
            echo "ERRO (ver ${OUTDIR}/run_${i}.stderr)"
            continue
        }

    echo "OK → ${OUTDIR}/run_${i}.json"
    # Espera 5s entre runs para o DUT estabilizar
    [[ $i -lt $REPS ]] && sleep 5
done

echo ""
echo "[$(date +%H:%M:%S)] Coleta concluída. Resultados em: ${OUTDIR}"
SCRIPT
    chmod +x /usr/local/bin/collect_baseline

    log "Scripts auxiliares criados:"
    log "  run_trex          — inicia o T-Rex (modo interativo)"
    log "  collect_baseline  — executa N repetições e salva JSONs"
}

# ─── 8. Resumo final ─────────────────────────────────────────────────────────

print_summary() {
    sep "Setup do T-Rex concluído"

    echo
    echo "┌─────────────────────────────────────────────────────────────────┐"
    echo "│                    RESUMO DO SETUP T-REX                       │"
    echo "├─────────────────────────────────────────────────────────────────┤"
    printf "│ %-63s│\n" " Versão instalada : T-Rex v${TREX_VERSION}"
    printf "│ %-63s│\n" " Diretório        : ${TREX_DIR}/v${TREX_VERSION}"
    printf "│ %-63s│\n" " Configuração     : /etc/trex_cfg.yaml"
    printf "│ %-63s│\n" " PORT0 (porta 0)  : ${PORT0}"
    printf "│ %-63s│\n" " PORT1 (porta 1)  : ${PORT1}"
    printf "│ %-63s│\n" " Perfis criados   : ${PROFILES_DIR}/"
    echo "├─────────────────────────────────────────────────────────────────┤"
    echo "│  PRÓXIMOS PASSOS:                                               │"
    echo "│                                                                 │"
    echo "│  1. Verificar configuração:                                     │"
    echo "│     cat /etc/trex_cfg.yaml                                      │"
    echo "│                                                                 │"
    echo "│  2. Testar link (modo servidor — mantém o processo rodando):    │"
    echo "│     run_trex --learn-mode                                       │"
    echo "│                                                                 │"
    echo "│  3. Enviar tráfego de teste (30s, 10% da linha):               │"
    echo "│     run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30     │"
    echo "│     --port 0 1                                                  │"
    echo "│                                                                 │"
    echo "│  4. Coletar baseline completo (10 repetições, 60s cada):       │"
    echo "│     collect_baseline ~/trex/profiles/steady_64b.py 60 100% 10  │"
    echo "│                                                                 │"
    echo "│  MÉTRICAS disponíveis nos JSONs de saída:                      │"
    echo "│    - throughput (Mpps, Gbps)                                    │"
    echo "│    - packet loss rate                                           │"
    echo "│    - latência: avg, min, max, percentis (µs)                   │"
    echo "└─────────────────────────────────────────────────────────────────┘"
    echo
    warn "Lembre-se: o DUT (servidor 1) deve estar rodando antes de enviar tráfego."
    warn "Comando no servidor 1: sudo ~/dpdk/build/examples/dpdk-l2fwd -l 0-15 -n 4 \\"
    warn "    -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1"
    echo
}

# ─── Main ─────────────────────────────────────────────────────────────────────

main() {
    log "Iniciando setup T-Rex v${TREX_VERSION} — $(uname -r) — $(date)"

    check_prerequisites
    install_trex
    fix_python38_compat
    detect_nics
    bind_nics
    generate_trex_config
    create_traffic_profiles
    create_run_helper
    print_summary
}

main "$@"
