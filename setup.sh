#!/bin/bash
# setup_dpdk.sh — Configura máquinas CloudLab para experimentos com DPDK
#
# Uso:
#   sudo ./setup_dpdk.sh [--skip-dpdk] [--dpdk-dir DIR] [--hugepages-1g N]
#
# Flags:
#   --skip-dpdk      Pula build/instalação do DPDK (só configura o sistema)
#   --dpdk-dir DIR   Diretório onde o DPDK será clonado/compilado (padrão: ~/dpdk)
#   --hugepages-1g N Número de hugepages de 1 GiB a reservar (padrão: 8)

set -euo pipefail

# ─── Configurações ────────────────────────────────────────────────────────────

DPDK_VERSION="v22.11"           # DPDK 22.11 LTS
DPDK_REPO="https://github.com/DPDK/dpdk.git"
DPDK_DIR="${HOME}/dpdk"
HUGEPAGES_1G=8                  # 8 × 1 GiB = 8 GiB reservados para DPDK
ISOLATED_CORES=""               # Ex: "2-15" — deixe vazio para detectar automaticamente
SKIP_DPDK=false

# ─── Parse de argumentos ──────────────────────────────────────────────────────

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-dpdk)     SKIP_DPDK=true ;;
        --dpdk-dir)      DPDK_DIR="$2"; shift ;;
        --hugepages-1g)  HUGEPAGES_1G="$2"; shift ;;
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

REBOOT_REQUIRED=false

# ─── 1. Dependências do sistema ───────────────────────────────────────────────

setup_dependencies() {
    sep "Instalando dependências"

    export DEBIAN_FRONTEND=noninteractive

    # Desabilita repos de terceiros com GPG inválido (comum em imagens CloudLab)
    for repo in /etc/apt/sources.list.d/*.list; do
        [[ -f "$repo" ]] || continue
        if grep -qi 'grafana\|influx\|elastic\|docker\|nodesource\|yarn\|microsoft' "$repo"; then
            warn "Desabilitando repo de terceiro: $(basename "$repo")"
            mv "$repo" "${repo}.disabled"
        fi
    done

    apt-get update -qq || apt-get update

    # Ferramentas de build
    apt-get install -y \
        build-essential git cmake ninja-build pkg-config \
        python3 python3-pip python3-pyelftools \
        libnuma-dev libpcap-dev zlib1g-dev \
        linux-headers-$(uname -r) \
        pciutils net-tools iproute2 \
        numactl hwloc \
        meson

    # Dependências para NIC Mellanox/NVIDIA ConnectX (mlx5 PMD)
    apt-get install -y \
        libibverbs-dev librdmacm-dev \
        ibverbs-utils rdma-core \
        libibumad-dev libibmad-dev || \
        warn "Pacotes RDMA não encontrados — PMD mlx5 pode não compilar"

    # Ferramentas de diagnóstico e performance
    # Nota: o pacote chama "linux-tools-$(uname -r)" — não existe pacote "perf" no Ubuntu 20.04.
    apt-get install -y linux-tools-$(uname -r) linux-tools-generic sysstat || \
        warn "linux-tools não instalado — verifique se linux-tools-$(uname -r) existe para este kernel"

    log "Dependências instaladas."
}

# ─── 2. Hugepages ─────────────────────────────────────────────────────────────

setup_hugepages() {
    sep "Configurando hugepages"

    local numa_nodes
    numa_nodes=$(ls /sys/devices/system/node/ | grep -c '^node[0-9]' || echo 1)
    local pages_per_node=$(( HUGEPAGES_1G / numa_nodes ))
    [[ $pages_per_node -lt 1 ]] && pages_per_node=1

    log "${HUGEPAGES_1G}x 1 GiB hugepages distribuídas em ${numa_nodes} nó(s) NUMA (${pages_per_node}/nó)"

    # Ativa no kernel agora (sem reboot)
    for node_dir in /sys/devices/system/node/node*/hugepages/hugepages-1048576kB; do
        [[ -f "${node_dir}/nr_hugepages" ]] || continue
        echo "$pages_per_node" > "${node_dir}/nr_hugepages"
    done

    # Fallback: hugepages globais se NUMA não disponível
    if [[ ! -d /sys/devices/system/node/node1 ]]; then
        echo "$HUGEPAGES_1G" > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
    fi

    # Monta hugetlbfs
    mkdir -p /dev/hugepages
    if ! mountpoint -q /dev/hugepages; then
        mount -t hugetlbfs nodev /dev/hugepages
    fi

    # Persistência via /etc/fstab
    if ! grep -q 'hugetlbfs' /etc/fstab; then
        echo 'nodev /dev/hugepages hugetlbfs defaults 0 0' >> /etc/fstab
    fi

    # Adiciona hugepages de 2 MiB também (fallback para algumas aplicações)
    echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages || true

    log "Hugepages configuradas:"
    grep -r '' /sys/kernel/mm/hugepages/hugepages-1048576kB/ 2>/dev/null | sed 's|/sys/kernel/mm/hugepages/hugepages-1048576kB/||'
}

# ─── 3. Parâmetros do kernel via GRUB ─────────────────────────────────────────

setup_grub() {
    sep "Configurando parâmetros do kernel (GRUB)"

    # Detecta cores disponíveis para isolamento (todos exceto o core 0)
    local total_cores
    total_cores=$(nproc --all)
    if [[ -z "$ISOLATED_CORES" && $total_cores -gt 1 ]]; then
        ISOLATED_CORES="1-$((total_cores - 1))"
    fi

    local GRUB_PARAMS=(
        # Hugepages de 1 GiB
        "default_hugepagesz=1G"
        "hugepagesz=1G"
        "hugepages=${HUGEPAGES_1G}"
        # Isolamento de CPU (evita interferência do OS scheduler)
        "isolcpus=${ISOLATED_CORES}"
        "nohz_full=${ISOLATED_CORES}"
        "rcu_nocbs=${ISOLATED_CORES}"
        # Desabilita mitigações (ganho de desempenho para benchmarks)
        "mitigations=off"
        "nospectre_v2"
        "nopti"
        # Desabilita estados de economia de energia
        "intel_idle.max_cstate=0"
        "processor.max_cstate=0"
        "idle=poll"
        # IOMMU para VFIO (necessário para dpdk com vfio-pci)
        "intel_iommu=on"
        "iommu=pt"
        # AMD alternativo
        "amd_iommu=on"
    )

    local param_string="${GRUB_PARAMS[*]}"

    # Backup do GRUB atual
    cp /etc/default/grub /etc/default/grub.bak

    # Atualiza GRUB_CMDLINE_LINUX
    if grep -q '^GRUB_CMDLINE_LINUX=' /etc/default/grub; then
        sed -i "s|^GRUB_CMDLINE_LINUX=.*|GRUB_CMDLINE_LINUX=\"${param_string}\"|" /etc/default/grub
    else
        echo "GRUB_CMDLINE_LINUX=\"${param_string}\"" >> /etc/default/grub
    fi

    update-grub 2>/dev/null || grub2-mkconfig -o /boot/grub2/grub.cfg 2>/dev/null || \
        warn "Não foi possível atualizar GRUB automaticamente — verifique manualmente"

    log "GRUB configurado. Parâmetros: ${param_string}"
    warn "É necessário reiniciar para que os parâmetros do kernel entrem em vigor."
    REBOOT_REQUIRED=true
}

# ─── 4. Turbo Boost e C-states (runtime, sem reboot) ─────────────────────────

setup_cpu_power() {
    sep "Configurando CPU (Turbo Boost, C-states, frequency scaling)"

    # Desabilita Turbo Boost — Intel
    if [[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]]; then
        echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
        log "Intel Turbo Boost desabilitado."
    fi

    # Desabilita Turbo Boost — AMD
    if [[ -f /sys/devices/system/cpu/cpufreq/boost ]]; then
        echo 0 > /sys/devices/system/cpu/cpufreq/boost
        log "AMD Boost desabilitado."
    fi

    # Força governor para performance
    for cpu in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_governor; do
        [[ -f "$cpu" ]] && echo performance > "$cpu" || true
    done

    # Desabilita C-states via /dev/cpu_dma_latency (mantém o fd aberto no background)
    # Isso é feito em runtime; a flag idle=poll no GRUB é a solução persistente.
    if command -v cpupower &>/dev/null; then
        cpupower idle-set -D 0 || true
    fi

    # Persiste o governor via rc.local
    if [[ -f /etc/rc.local ]]; then
        grep -q 'scaling_governor' /etc/rc.local || \
            sed -i '/^exit 0/i for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$cpu" 2>/dev/null || true; done' /etc/rc.local
    fi

    log "CPU configurada para máximo desempenho."
}

# ─── 5. Hyperthreading ────────────────────────────────────────────────────────

disable_hyperthreading() {
    sep "Desabilitando Hyperthreading"

    local ht_active=false
    if [[ -f /sys/devices/system/cpu/smt/active ]]; then
        [[ $(cat /sys/devices/system/cpu/smt/active) == "1" ]] && ht_active=true
    fi

    if $ht_active; then
        echo off > /sys/devices/system/cpu/smt/control
        log "Hyperthreading desabilitado (runtime)."
        warn "Adicione 'nosmt' ao GRUB_CMDLINE_LINUX para persistência entre reboots."
    else
        log "Hyperthreading já está desabilitado ou não disponível."
    fi
}

# ─── 6. Módulos de kernel para DPDK ──────────────────────────────────────────

setup_kernel_modules() {
    sep "Carregando módulos do kernel"

    # VFIO (preferível ao UIO por ser mais seguro e suportado no Linux moderno)
    modprobe vfio || warn "Módulo vfio não disponível"
    modprobe vfio-pci || warn "Módulo vfio-pci não disponível"

    # Habilita VFIO sem IOMMU (para ambientes sem VT-d/AMD-Vi ativado no BIOS)
    if [[ -f /sys/module/vfio/parameters/enable_unsafe_noiommu_mode ]]; then
        echo Y > /sys/module/vfio/parameters/enable_unsafe_noiommu_mode
        warn "VFIO rodando em modo noiommu — ative IOMMU no BIOS para produção."
    fi

    # UIO (fallback)
    modprobe uio || true
    modprobe uio_pci_generic || true

    # Persistência
    cat > /etc/modules-load.d/dpdk.conf << 'EOF'
vfio
vfio-pci
uio
uio_pci_generic
EOF

    log "Módulos carregados."
}

# ─── 7. Build do DPDK ─────────────────────────────────────────────────────────

install_dpdk() {
    sep "Clonando e compilando DPDK ${DPDK_VERSION}"

    if [[ -d "${DPDK_DIR}" ]]; then
        warn "Diretório ${DPDK_DIR} já existe — pulando clone."
    else
        git clone --depth 1 --branch "${DPDK_VERSION}" "${DPDK_REPO}" "${DPDK_DIR}"
    fi

    cd "${DPDK_DIR}"

    # Configura com meson (habilita mlx5 para ConnectX)
    meson setup build \
        -Dplatform=native \
        -Dmax_ethports=32 \
        -Denable_kmods=true \
        -Dexamples=all \
        --prefix=/usr/local

    ninja -C build -j"$(nproc)"
    ninja -C build install
    ldconfig

    log "DPDK instalado em /usr/local"
    log "Variáveis úteis:"
    echo "  export PKG_CONFIG_PATH=/usr/local/lib/x86_64-linux-gnu/pkgconfig"
    echo "  export LD_LIBRARY_PATH=/usr/local/lib/x86_64-linux-gnu"
}

# ─── 8. Utilitário para bind de NICs ─────────────────────────────────────────

install_nic_bind_helper() {
    sep "Instalando helper para bind de NICs"

    # Script wrapper em volta do dpdk-devbind.py
    cat > /usr/local/bin/dpdk-bind << 'SCRIPT'
#!/bin/bash
# Wrapper para dpdk-devbind.py
# Uso: dpdk-bind --status
#      dpdk-bind --bind=vfio-pci <PCI_ADDR>
#      dpdk-bind --bind=mlx5_core <PCI_ADDR>   # para ConnectX — bifurcated mode

DEVBIND=$(find /usr/local/share/dpdk -name 'dpdk-devbind.py' 2>/dev/null | head -1)
DEVBIND=${DEVBIND:-$(which dpdk-devbind.py 2>/dev/null)}

if [[ -z "$DEVBIND" ]]; then
    echo "dpdk-devbind.py não encontrado. Verifique a instalação do DPDK." >&2
    exit 1
fi

python3 "$DEVBIND" "$@"
SCRIPT

    chmod +x /usr/local/bin/dpdk-bind
    log "Use 'dpdk-bind --status' para listar NICs disponíveis."
}

# ─── 9. Resumo final ─────────────────────────────────────────────────────────

print_summary() {
    sep "Setup concluído"

    echo
    echo "┌─────────────────────────────────────────────────────────────────┐"
    echo "│                     RESUMO DO SETUP                            │"
    echo "├─────────────────────────────────────────────────────────────────┤"
    printf "│ %-63s│\n" " Hugepages 1G reservadas : ${HUGEPAGES_1G}"
    printf "│ %-63s│\n" " Cores isolados (GRUB)   : ${ISOLATED_CORES:-'nenhum'}"
    printf "│ %-63s│\n" " DPDK instalado          : $( $SKIP_DPDK && echo 'não (--skip-dpdk)' || echo ${DPDK_VERSION} )"
    printf "│ %-63s│\n" " Módulos carregados      : vfio-pci, uio_pci_generic"
    echo "├─────────────────────────────────────────────────────────────────┤"
    echo "│  PRÓXIMOS PASSOS:                                               │"
    echo "│  1. Reiniciar a máquina para aplicar parâmetros do GRUB        │"
    echo "│  2. dpdk-bind --status          # listar NICs                  │"
    echo "│  3. dpdk-bind --bind=vfio-pci <BDF>  # bind da NIC            │"
    echo "│     (ConnectX-5: não precisa de bind — use PMD mlx5)           │"
    echo "│  4. Verificar hugepages: cat /proc/meminfo | grep Huge         │"
    echo "└─────────────────────────────────────────────────────────────────┘"
    echo

    if $REBOOT_REQUIRED; then
        warn "REBOOT NECESSÁRIO para aplicar: hugepages persistentes, isolcpus, mitigações."
        echo
        read -rp "Reiniciar agora? [s/N] " ans
        [[ "${ans,,}" == "s" ]] && reboot
    fi
}

# ─── Main ─────────────────────────────────────────────────────────────────────

main() {
    log "Iniciando setup DPDK — $(uname -r) — $(date)"

    setup_dependencies
    setup_hugepages
    setup_grub
    setup_cpu_power
    disable_hyperthreading
    setup_kernel_modules

    if ! $SKIP_DPDK; then
        install_dpdk
    fi

    install_nic_bind_helper
    print_summary
}

main "$@"
