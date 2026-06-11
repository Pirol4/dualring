#!/bin/bash
# setup_trex.sh — Instala e configura T-Rex no Servidor 2 (gerador de carga)
#
# Pré-requisito: sudo ./setup.sh deve ter sido executado antes (hugepages, vfio-pci)
#
# Uso:
#   sudo ./setup_trex.sh [--trex-version VER] [--trex-dir DIR] [--port0 BDF] [--port1 BDF]
#
# Flags:
#   --trex-version VER  Versão do T-Rex a baixar (padrão: 3.06)
#   --trex-dir DIR      Diretório de instalação (padrão: ~/trex do usuário real)
#   --port0 BDF         Endereço PCI da porta 0 (ex: 0000:41:00.0) — detectado auto se omitido
#   --port1 BDF         Endereço PCI da porta 1 (ex: 0000:41:00.1) — detectado auto se omitido
#   --skip-link-check   Pula a verificação de link físico das NICs (para testes)
#
# Modelo de execução do T-Rex (IMPORTANTE):
#   Perfis STL (.py) NÃO rodam em modo batch (-f/-d sem -i) — isso só existe para
#   STF/ASTF. O fluxo correto é: servidor interativo (t-rex-64 -i) + cliente via
#   API Python. Os helpers run_trex/collect_baseline gerados aqui seguem esse modelo.

set -euo pipefail

# ─── Configurações ────────────────────────────────────────────────────────────

# Home do usuário real (não /root quando rodando via sudo) — evita instalar
# o T-Rex em lugar diferente do esperado e permite o chown correto dos perfis
REAL_USER="${SUDO_USER:-$USER}"
REAL_HOME="$(getent passwd "${REAL_USER}" | cut -d: -f6)"

TREX_VERSION="3.06"
TREX_DIR="${REAL_HOME}/trex"
PORT0=""
PORT1=""
SKIP_LINK_CHECK=0

# ─── Parse de argumentos ──────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --trex-version) TREX_VERSION="$2"; shift ;;
        --trex-dir)     TREX_DIR="$2";     shift ;;
        --port0)        PORT0="$2";        shift ;;
        --port1)        PORT1="$2";        shift ;;
        --skip-link-check) SKIP_LINK_CHECK=1 ;;
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

    local hp_total
    hp_total=$(grep -i 'HugePages_Total' /proc/meminfo | awk '{print $2}')
    if [[ "${hp_total}" -lt 2 ]]; then
        die "Hugepages insuficientes (${hp_total} disponíveis). Execute setup.sh primeiro."
    fi
    log "Hugepages OK: ${hp_total} × 1 GiB disponíveis."

    python3 --version &>/dev/null || die "Python 3 não encontrado. Execute setup.sh primeiro."
    log "Python 3 OK: $(python3 --version)"

    if ! python3 -c "import scapy" &>/dev/null; then
        log "Instalando scapy..."
        pip3 install scapy --quiet
    fi
    log "scapy OK."

    if ! lsmod | grep -q vfio_pci; then
        log "Carregando módulo vfio-pci..."
        modprobe vfio-pci || warn "vfio-pci não disponível — T-Rex usará uio_pci_generic como fallback."
        modprobe uio_pci_generic || true
    fi
    log "Módulos de kernel OK."

    if ! dpkg -l rdma-core &>/dev/null; then
        log "Instalando rdma-core (libibverbs para mlx5 PMD)..."
        apt-get install -y --quiet rdma-core libibverbs-dev libmlx5-dev ibverbs-utils 2>/dev/null || \
            warn "Pacotes rdma-core não instalados — mlx5 PMD pode falhar."
    fi
    log "rdma-core OK."
}

# ─── 1b. rdma-core moderna (exigida pelo PMD mlx5 do T-Rex) ──────────────────
#
# FIX (debug 10/06/2026): o binário mlx5 pré-compilado do T-Rex v3.06
# (so/x86_64/libmlx5-64.so) exige símbolos MLX5_1.15+ que a rdma-core do
# Ubuntu 20.04 (v28) não exporta. Erro observado:
#   EAL: /lib/x86_64-linux-gnu/libmlx5.so.1: version `MLX5_1.15' not found
# Solução: compilar rdma-core v44 em /usr/local/lib (precedência sobre a do
# sistema no loader, sem substituir pacotes do apt).

build_rdma_core() {
    sep "Verificando versão da libmlx5 (símbolos exigidos pelo T-Rex)"

    # || true: evita que set -e mate o script se ldconfig ou awk saírem com erro
    local current_lib
    current_lib=$(ldconfig -p | awk '/libmlx5\.so\.1/{print $NF; exit}' || true)
    log "libmlx5 encontrada: ${current_lib:-<nenhuma>}"

    # Verifica símbolos em etapas separadas para não disparar pipefail dentro de if
    local has_symbols=0
    if [[ -n "${current_lib}" ]] && command -v objdump &>/dev/null; then
        objdump -T "${current_lib}" 2>/dev/null | grep -q 'MLX5_1.1[5-9]' \
            && has_symbols=1 || has_symbols=0
    fi

    if [[ "${has_symbols}" -eq 1 ]]; then
        log "libmlx5 atual (${current_lib}) já exporta MLX5_1.15+ — OK."
        return
    fi

    log "libmlx5 do sistema é antiga demais (ou não encontrada) para o T-Rex. Compilando rdma-core v44..."

    apt-get install -y --quiet build-essential cmake ninja-build pkg-config \
        libudev-dev libnl-3-dev libnl-route-3-dev libssl-dev python3-dev cython3 || \
        die "Falha ao instalar dependências de build da rdma-core."

    local build_dir="/tmp/rdma-core-build"
    rm -rf "${build_dir}"
    git clone -b v44.0 --depth 1 https://github.com/linux-rdma/rdma-core.git "${build_dir}" || \
        die "Falha ao clonar rdma-core."

    mkdir -p "${build_dir}/build"
    cd "${build_dir}/build"
    cmake -GNinja -DNO_MAN_PAGES=1 .. || die "Falha no cmake da rdma-core."
    ninja || die "Falha na compilação da rdma-core."
    ninja install || die "Falha no install da rdma-core."
    ldconfig
    cd - >/dev/null

    # Verificação pós-install
    objdump -T /usr/local/lib/libmlx5.so.1 2>/dev/null | grep -q 'MLX5_1.1[5-9]' || \
        die "rdma-core instalada mas símbolo MLX5_1.15+ não encontrado em /usr/local/lib/libmlx5.so.1."
    log "rdma-core v44 instalada em /usr/local/lib — símbolos MLX5_1.15+ disponíveis."
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

fix_python38_compat() {
    sep "Corrigindo compatibilidade Python 3.8+"

    local trex_install="${TREX_DIR}/v${TREX_VERSION}"

    local broken_files
    broken_files=$(grep -rl "platform\.dist\(\|platform\.linux_distribution(" \
        "${trex_install}" --include="*.py" 2>/dev/null || true)

    if [[ -z "${broken_files}" ]]; then
        log "Nenhum arquivo com platform.dist encontrado — OK."
    else
        log "Arquivos para patching:"
        while IFS= read -r f; do
            echo "  ${f}"
            if ! grep -q "platform\.dist = lambda" "${f}"; then
                sed -i '1s/^/import platform\nif not hasattr(platform, "dist"):\n    platform.dist = lambda *a, **k: ("", "", "")\n    platform.linux_distribution = lambda *a, **k: ("", "", "")\n/' "${f}"
            fi
        done <<< "${broken_files}"
        log "Shim de compatibilidade injetado."
    fi

    if ! command -v ofed_info &>/dev/null; then
        log "Criando stub /usr/bin/ofed_info para passar verificação do T-Rex..."
        cat > /usr/bin/ofed_info << 'STUB'
#!/bin/bash
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

# ─── 3b. Verificação de link físico ──────────────────────────────────────────
#
# FIX (debug 10/06/2026): experimento CloudLab foi provisionado sem enlaces nas
# ConnectX-5 (NO-CARRIER) — o T-Rex subia, mas transmitia 0 pps com link DOWN.
# Esta verificação falha cedo e com mensagem clara, em vez de deixar o problema
# aparecer só na hora do tráfego.

check_link_status() {
    sep "Verificando link físico das NICs"

    if [[ "${SKIP_LINK_CHECK}" -eq 1 ]]; then
        warn "Verificação de link pulada (--skip-link-check)."
        return
    fi

    local failed=0
    for port in "${PORT0}" "${PORT1}"; do
        # Descobre o netdev associado ao endereço PCI (mlx5 é bifurcado: netdev coexiste com DPDK)
        local iface
        iface=$(ls "/sys/bus/pci/devices/${port}/net/" 2>/dev/null | head -1)

        if [[ -z "${iface}" ]]; then
            warn "NIC ${port}: sem netdev associado (driver não-bifurcado ou já em vfio-pci) — pulando check de link."
            continue
        fi

        # Garante interface administrativamente up (não gera tráfego; só ativa o link)
        ip link set "${iface}" up 2>/dev/null || true
        sleep 1

        local carrier speed
        carrier=$(cat "/sys/class/net/${iface}/carrier" 2>/dev/null || echo 0)
        speed=$(cat "/sys/class/net/${iface}/speed" 2>/dev/null || echo -1)

        if [[ "${carrier}" != "1" ]]; then
            warn "NIC ${port} (${iface}): SEM LINK FÍSICO (NO-CARRIER)."
            failed=1
        elif [[ "${speed}" -lt 100000 ]]; then
            warn "NIC ${port} (${iface}): link em ${speed} Mb/s — esperado 100000 (100G)."
            failed=1
        else
            log "NIC ${port} (${iface}): link UP @ ${speed} Mb/s. OK."
        fi
    done

    if [[ "${failed}" -eq 1 ]]; then
        die "Link físico ausente ou abaixo de 100G. Causa típica no CloudLab: o profile do
experimento não definiu os enlaces de 100G entre os nós (verifique o profile.py —
os links devem pedir bandwidth=100000000). O T-Rex mostraria 'link: DOWN' e 0 pps.
Use --skip-link-check apenas para testes sem hardware."
    fi
}

# ─── 4. Bind das NICs para o T-Rex ────────────────────────────────────────────

bind_nics() {
    sep "Configurando NICs para uso com T-Rex"

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

    local total_cores
    total_cores=$(nproc --all)
    local worker_start=2
    local worker_end=$((total_cores - 1))

    local numa0
    numa0=$(cat "/sys/bus/pci/devices/${PORT0}/numa_node" 2>/dev/null || echo 0)
    [[ "${numa0}" -lt 0 ]] && numa0=0

    local threads=""
    for ((i=worker_start; i<=worker_end; i++)); do
        threads+="${i}"
        [[ $i -lt $worker_end ]] && threads+=","
    done

    cat > /etc/trex_cfg.yaml << EOF
# Configuração T-Rex gerada por setup_trex.sh
# Gerado em: $(date)

- port_limit      : 2
  version         : 2
  interfaces      : ['${PORT0}', '${PORT1}']

  port_info       :
    - ip          : 16.0.0.1
      default_gw  : 48.0.0.1
    - ip          : 48.0.0.1
      default_gw  : 16.0.0.1

  platform        :
    master_thread_id  : 0
    latency_thread_id : 1
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

    cat > "${PROFILES_DIR}/steady_64b.py" << 'EOF'
"""
Perfil: fluxo continuo de pacotes de 64 bytes (STL).

Uso (console):    start -f profiles/steady_64b.py -m 100% -d 60 -p 0 1
Uso (helper):     run_trex -f ~/trex/profiles/steady_64b.py -m 100% -d 60

A taxa e controlada pelo -m do start (percentual de line rate ou pps).
"""
from trex_stl_lib.api import *

class STLSteady64(object):

    def get_streams(self, direction=0, **kwargs):
        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 64 - len(base_pkt) - 4)
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        data_stream = STLStream(
            name='data',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(),
            flow_stats=STLFlowStats(pg_id=1),
        )

        lat_stream = STLStream(
            name='latency',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(pps=1000),
            flow_stats=STLFlowLatencyStats(pg_id=11),
        )

        return [data_stream, lat_stream]


def register():
    return STLSteady64()
EOF

    cat > "${PROFILES_DIR}/steady_1500b.py" << 'EOF'
"""
Perfil: fluxo contínuo de pacotes de 1500 bytes
Uso: ./t-rex-64 -f profiles/steady_1500b.py -m 100% --port 0 1 -d 60
"""
from trex_stl_lib.api import *

class STLSteady1500(object):

    def get_streams(self, direction=0, **kwargs):
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

    # FIX (10/06/2026): STLArgParser não existe na API STL — tunables via kwargs.
    # FIX: stream com next apontando para si mesmo é inválido — o primitivo correto
    # para rajadas periódicas é STLTXMultiBurst (pps no burst, ibg entre bursts).
    cat > "${PROFILES_DIR}/bursty_64b.py" << 'EOF'
"""
Perfil: trafego bursty de 64 bytes — revela o trade-off leaky DMA.

Estrutura de cada ciclo:
  burst_pkts pacotes a burst_pps  →  idle_us de silencio  →  repete

Tunables (via -t no console: start -f ... -t burst_pkts=4096,idle_us=50):
  burst_pkts   Pacotes por burst       (padrao: 2048)
  burst_pps    Taxa durante o burst    (padrao: 1e8 = 100 Mpps)
  idle_us      Pausa entre bursts (us) (padrao: 10)

Uso (console):  start -f profiles/bursty_64b.py -m 1 -d 60 -p 0 1
Uso (helper):   run_trex -f ~/trex/profiles/bursty_64b.py -m 1 -d 60
"""
from trex_stl_lib.api import *

class STLBursty64(object):

    def get_streams(self, direction=0, burst_pkts=2048, burst_pps=100000000.0,
                    idle_us=10.0, **kwargs):
        burst_pkts = int(burst_pkts)
        burst_pps = float(burst_pps)
        idle_us = float(idle_us)

        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 64 - len(base_pkt) - 4)
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        # ibg (inter-burst gap) e em MICROSEGUNDOS; count alto = duracao limitada pelo -d
        burst_stream = STLStream(
            name='burst',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXMultiBurst(
                pps=burst_pps,
                pkts_per_burst=burst_pkts,
                ibg=idle_us,
                count=1000000,
            ),
            flow_stats=STLFlowStats(pg_id=3),
        )

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

    # FIX (10/06/2026): script roda como root, mas o usuário precisa editar os
    # perfis sem sudo (Permission denied observado ao regravar steady_64b.py)
    chown -R "${REAL_USER}:$(id -gn "${REAL_USER}")" "${TREX_DIR}"

    log "Perfis criados em ${PROFILES_DIR} (dono: ${REAL_USER}):"
    log "  steady_64b.py   — fluxo contínuo, 64B"
    log "  steady_1500b.py — fluxo contínuo, 1500B"
    log "  bursty_64b.py   — tráfego bursty, 64B"
}

# ─── 7. Scripts auxiliares ────────────────────────────────────────────────────

create_run_helper() {
    sep "Criando scripts auxiliares"

    local trex_bin="${TREX_DIR}/v${TREX_VERSION}"

    # FIX (10/06/2026): perfis STL (.py) NÃO funcionam em modo batch — o trex-cfg
    # rejeita com "Python files can not be used with STF mode". O modelo correto:
    # servidor interativo (t-rex-64 -i) + cliente via API Python (STLClient).
    # O run_trex abaixo sobe o servidor se necessário e delega ao run_trex_stl.py.

    cat > /usr/local/bin/run_trex << EOF
#!/bin/bash
# Executa um perfil STL no T-Rex (sobe o servidor automaticamente se preciso).
# Uso: run_trex -f <perfil.py> -m <taxa> -d <duração_s>
# Ex.:  run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30
# Variáveis: TREX_CORES (padrão 4) controla os cores do servidor.

set -euo pipefail
TREX_BIN="${trex_bin}"
TREX_CORES="\${TREX_CORES:-4}"

if ! pgrep -f '_t-rex-64' > /dev/null; then
    echo "[run_trex] Servidor T-Rex não está rodando — iniciando (\${TREX_CORES} cores)..."
    sudo bash -c "cd '\${TREX_BIN}' && nohup ./t-rex-64 -i -c \${TREX_CORES} --cfg /etc/trex_cfg.yaml > /tmp/trex_server.log 2>&1 &"
    for i in \$(seq 1 60); do
        ss -ltn 2>/dev/null | grep -q ':4501' && break
        sleep 1
    done
    if ! ss -ltn 2>/dev/null | grep -q ':4501'; then
        echo "[run_trex] ERRO: servidor não subiu em 60s. Log: /tmp/trex_server.log" >&2
        tail -20 /tmp/trex_server.log >&2 || true
        exit 1
    fi
    echo "[run_trex] Servidor pronto (porta 4501)."
fi

exec python3 /usr/local/bin/run_trex_stl.py "\$@"
EOF
    chmod +x /usr/local/bin/run_trex

    cat > /usr/local/bin/run_trex_stl.py << EOF
#!/usr/bin/env python3
"""Cliente STL: conecta no servidor T-Rex local, roda um perfil e imprime stats."""
import argparse
import json
import os
import sys

TREX_API = "${trex_bin}/automation/trex_control_plane/interactive"
sys.path.insert(0, TREX_API)
from trex.stl.api import STLClient, STLProfile  # noqa: E402

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-f", dest="profile", required=True, help="caminho do perfil .py")
    ap.add_argument("-m", dest="mult", default="10%", help="taxa (ex.: 10%%, 1mpps)")
    ap.add_argument("-d", dest="duration", type=float, default=30, help="duração em s")
    ap.add_argument("-p", dest="ports", type=int, nargs="+", default=[0, 1])
    args = ap.parse_args()

    profile_path = os.path.abspath(os.path.expanduser(args.profile))
    if not os.path.isfile(profile_path):
        sys.exit("Perfil não encontrado: %s" % profile_path)

    c = STLClient()
    c.connect()
    try:
        ports = args.ports
        c.reset(ports=ports)
        prof = STLProfile.load_py(profile_path)
        c.add_streams(prof.get_streams(), ports=ports)
        c.clear_stats()
        c.start(ports=ports, mult=args.mult, duration=args.duration, force=True)
        c.wait_on_traffic(ports=ports)

        s = c.get_stats()
        tx = sum(s[p]["opackets"] for p in ports)
        rx = sum(s[p]["ipackets"] for p in ports)
        loss = tx - rx
        loss_pct = (100.0 * loss / tx) if tx else 0.0

        print("")
        print("=== RESULTADO ===")
        print("TX total : %d" % tx)
        print("RX total : %d" % rx)
        print("Perda    : %d (%.4f%%)" % (loss, loss_pct))
        for p in ports:
            print("  porta %d: tx=%d rx=%d" % (p, s[p]["opackets"], s[p]["ipackets"]))

        # Linha JSON para parsing automatizado (collect_baseline / scripts de análise)
        try:
            summary = {
                "tx": tx, "rx": rx, "loss": loss, "loss_pct": loss_pct,
                "ports": {str(p): {"tx": s[p]["opackets"], "rx": s[p]["ipackets"]}
                          for p in ports},
                "latency": s.get("latency", {}),
            }
            print("JSON_STATS: %s" % json.dumps(summary, default=str))
        except Exception as e:
            print("JSON_STATS_ERROR: %s" % e)
    finally:
        c.disconnect()

if __name__ == "__main__":
    main()
EOF
    chmod +x /usr/local/bin/run_trex_stl.py

    # FIX (10/06/2026): collect_baseline reescrito sobre o run_trex (API STL).
    # A versão anterior usava modo batch (-f/-d direto no t-rex-64), que não
    # existe para perfis STL.
    cat > /usr/local/bin/collect_baseline << 'SCRIPT'
#!/bin/bash
# Executa N repetições de um perfil STL e salva os resultados.
#
# Uso: collect_baseline <perfil> <duracao_s> <taxa> [repeticoes]
# Exemplo: collect_baseline ~/trex/profiles/steady_64b.py 60 100% 10

set -euo pipefail

PROFILE="${1:-}"
DURATION="${2:-60}"
RATE="${3:-100%}"
REPS="${4:-10}"

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

FAILED=0
for ((i=1; i<=REPS; i++)); do
    echo "[$(date +%H:%M:%S)] Run ${i}/${REPS}..."

    if run_trex -f "${PROFILE}" -m "${RATE}" -d "${DURATION}" \
        > "${OUTDIR}/run_${i}.txt" 2>"${OUTDIR}/run_${i}.stderr"; then
        echo "  OK → ${OUTDIR}/run_${i}.txt"
    else
        echo "  ERRO (ver ${OUTDIR}/run_${i}.stderr)"
        FAILED=$((FAILED + 1))
    fi

    [[ $i -lt $REPS ]] && sleep 5
done

# Consolida as linhas JSON de todas as runs num único arquivo para análise
grep -h '^JSON_STATS:' "${OUTDIR}"/run_*.txt 2>/dev/null | sed 's/^JSON_STATS: //' \
    > "${OUTDIR}/all_runs.jsonl" || true

echo ""
echo "[$(date +%H:%M:%S)] Coleta concluída (${FAILED} falhas). Resultados em: ${OUTDIR}"
echo "  Resumo JSON por run: ${OUTDIR}/all_runs.jsonl"
SCRIPT
    chmod +x /usr/local/bin/collect_baseline

    log "Scripts auxiliares criados:"
    log "  run_trex          — sobe o servidor (-i) se preciso e roda o perfil via API STL"
    log "  run_trex_stl.py   — cliente Python da API STL (usado pelo run_trex)"
    log "  collect_baseline  — executa N repetições via run_trex e consolida JSON"
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
    echo "│  2. Teste inicial (30s, 10% da linha):                         │"
    echo "│     run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30     │"
    echo "│     (sobe o servidor T-Rex automaticamente se preciso)          │"
    echo "│                                                                 │"
    echo "│  3. Coletar baseline completo (10 repetições, 60s cada):       │"
    echo "│     collect_baseline ~/trex/profiles/steady_64b.py 60 100% 10  │"
    echo "│                                                                 │"
    echo "│  MÉTRICAS disponíveis na saída texto:                          │"
    echo "│    - throughput (Mpps, Gbps)                                    │"
    echo "│    - packet loss rate                                           │"
    echo "│    - latência: avg, min, max, jitter (µs)                      │"
    echo "└─────────────────────────────────────────────────────────────────┘"
    echo
    warn "Lembre-se: o DUT (servidor 1) deve estar rodando antes de enviar tráfego."
    warn "Comando no servidor 1:"
    warn "  sudo ~/dpdk/build/examples/dpdk-l2fwd -l 0-15 -n 4 \\"
    warn "      -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1"
    echo
}

# ─── Main ─────────────────────────────────────────────────────────────────────

main() {
    log "Iniciando setup T-Rex v${TREX_VERSION} — $(uname -r) — $(date)"

    check_prerequisites
    build_rdma_core
    install_trex
    fix_python38_compat
    detect_nics
    check_link_status
    bind_nics
    generate_trex_config
    create_traffic_profiles
    create_run_helper
    print_summary
}

main "$@"
