# Progresso do TCC — Log de Setup e Experimentos

## Visão geral do projeto

O objetivo é implementar um sistema de **alocação dinâmica de buffers na LLC** usando uma estrutura de anéis duplos de recepção sobre DPDK/AF_XDP, e comparar o desempenho contra:

1. **DPDK padrão** (l2fwd) — baseline
2. **Shring** — estado da arte (array ring compartilhado, não dinâmico)
3. **Implementação própria** — anéis duplos dinâmicos (contribuição do TCC)

O problema central é o trade-off do **Leaky DMA**: NICs modernas fazem DMA diretamente na LLC via DDIO. Anéis grandes absorvem bursts mas expulsam dados úteis da cache; anéis pequenos preservam a cache mas causam perda de pacotes em picos de tráfego.

---

## Infraestrutura

### CloudLab

Experimentos rodam em duas máquinas **d6515** alocadas no [CloudLab](https://www.cloudlab.us/), conectadas **back-to-back** pelas NICs de alta velocidade.

| Servidor | Hostname | Papel |
|---|---|---|
| Servidor 1 | `server.pirola-307281.aos-ufmg-dcc831-pg0.utah.cloudlab.us` | DUT (sistema avaliado) |
| Servidor 2 | *(mesmo experimento CloudLab)* | Gerador de carga (T-Rex) |

### Hardware real (d6515)

> **Nota:** O CLAUDE.md descreve o hardware do artigo Shring (Intel Xeon Silver 4216, LLC 22 MiB). O hardware real das d6515 no CloudLab é diferente:

| Componente | Valor |
|---|---|
| CPU | AMD EPYC 7452 — 32 cores físicos, 64 threads (HT ativo) |
| Frequência | 3340 MHz |
| L1d / L1i | 1 MiB cada |
| L2 | 16 MiB |
| L3 (LLC) | 128 MiB |
| RAM | 125 GiB |
| NUMA nodes | 1 |
| NICs | 2x NVIDIA/Mellanox ConnectX-5 Ex (MT28800) — 100 GbE |
| PCIe NICs | `0000:41:00.0` e `0000:41:00.1` |
| OS | Ubuntu 20.04 LTS (Focal Fossa) |
| Kernel | Linux 5.4.0-100-generic |

---

## O que foi feito

### Servidor 1 — configuração completa

#### 1. Setup do sistema (`setup_dpdk.sh`)

Executado o script `setup_dpdk.sh` que realizou:

**Dependências instaladas:**
- Ferramentas de build: `build-essential`, `git`, `cmake`, `ninja-build`, `meson`, `pkg-config`
- Python: `python3`, `python3-pip`, `python3-pyelftools`
- NUMA/PCIe: `libnuma-dev`, `libpcap-dev`, `pciutils`, `numactl`, `hwloc`
- RDMA/Mellanox: `libibverbs-dev`, `librdmacm-dev`, `rdma-core`, `ibverbs-utils`
- Headers do kernel: `linux-headers-5.4.0-100-generic`

**Hugepages configuradas:**
- 8× hugepages de 1 GiB reservadas
- Montadas em `/dev/hugepages` (hugetlbfs)
- Persistência via `/etc/fstab`
- Estado atual: 8 total, 6 livres (2 em uso pelo DPDK)

**Parâmetros do kernel (GRUB) — ativos após reboot:**

```
default_hugepagesz=1G
hugepagesz=1G
hugepages=8
isolcpus=1-63          # isola todos os cores exceto o 0 para o DPDK
nohz_full=1-63
rcu_nocbs=1-63
mitigations=off        # desabilita Spectre/Meltdown (ganho de desempenho)
nospectre_v2
nopti
intel_idle.max_cstate=0
processor.max_cstate=0
idle=poll              # desabilita C-states
intel_iommu=on
iommu=pt
amd_iommu=on           # IOMMU ativo (necessário para vfio-pci)
```

> O reboot já foi realizado — todos os parâmetros acima estão **ativos** em `/proc/cmdline`.

**Módulos do kernel carregados:**
- `uio`, `uio_pci_generic` — carregados
- `vfio`, `vfio-pci` — não carregados (desnecessário: ConnectX-5 usa modo bifurcado com `mlx5_core`)

**NICs — estado atual:**
```
41:00.0  ConnectX-5 Ex  →  driver: mlx5_core  (modo bifurcado — correto para DPDK)
41:00.1  ConnectX-5 Ex  →  driver: mlx5_core  (modo bifurcado — correto para DPDK)
```

No modo bifurcado, a NIC permanece no driver do kernel (`mlx5_core`) **e** o DPDK acessa via PMD mlx5 simultaneamente. Não é necessário fazer `bind` para `vfio-pci`.

**Configurações de CPU (runtime, sem persistência entre reboots):**
- Turbo Boost desabilitado
- Governor de frequência definido como `performance`
- C-states desabilitados via `cpupower`
- Hyperthreading: desabilitado em runtime pelo script, mas **não persistiu** — a máquina tem 64 CPUs lógicas (32 cores × 2 threads). Para persistir, adicionar `nosmt` ao GRUB.

#### 2. Build do DPDK

- **Versão:** DPDK 22.11.0 (LTS)
- **Repositório:** clonado em `~/dpdk`
- **Compilado com:**
  ```bash
  meson setup build \
      -Dplatform=native \
      -Dmax_ethports=32 \
      -Denable_kmods=true \
      --prefix=/usr/local
  ninja -C build -j$(nproc)
  ninja -C build install
  ```
- **Toolchain:** GCC 9.3.0, Meson 0.53.2, Ninja 1.10.0

**Binários disponíveis:**
| Binário | Caminho | Status |
|---|---|---|
| `dpdk-testpmd` | `~/dpdk/build/app/dpdk-testpmd` | ✓ compilado (36 MiB) |
| `dpdk-l2fwd` | `~/dpdk/build/examples/dpdk-l2fwd` | ✓ compilado (35 MiB) |

O `l2fwd` não estava incluído no build inicial (precisa de `-Dexamples=l2fwd`). Foi recompilado com:
```bash
sudo meson setup --reconfigure -Dexamples=l2fwd ~/dpdk/build ~/dpdk
sudo ninja -C ~/dpdk/build examples/dpdk-l2fwd
```

#### 3. Validação das NICs

Executado `dpdk-testpmd` para confirmar que o DPDK consegue acessar as ConnectX-5:

```bash
sudo ~/dpdk/build/app/dpdk-testpmd \
    -l 0-1 -n 4 -a 0000:41:00.0 \
    -- --stats-period 1
```

Resultado: PMD `mlx5_pci` carregado, MAC `1C:34:DA:41:CF:CC` detectada, estatísticas RX/TX rodando sem erros.

---

### Servidor 2 — em andamento

| Passo | Status |
|---|---|
| Rodar `setup_dpdk.sh` | Em andamento |
| Reboot para aplicar hugepages/isolcpus | Pendente |
| Rodar `setup_trex.sh` | Pendente |
| Validar link back-to-back | Pendente |

---

## Scripts disponíveis

### `setup_dpdk.sh`

Configura o sistema para uso com DPDK:
- Instala dependências
- Configura hugepages (1 GiB)
- Atualiza GRUB com parâmetros de desempenho
- Desabilita Turbo Boost, C-states, Hyperthreading
- Carrega módulos vfio-pci / uio
- Clona e compila DPDK 22.11 LTS

```bash
sudo ./setup_dpdk.sh [--skip-dpdk] [--dpdk-dir DIR] [--hugepages-1g N]
```

### `setup_trex.sh`

Instala e configura o T-Rex no Servidor 2:
- Baixa T-Rex v3.06
- Corrige compatibilidade Python 3.8+ (`platform.dist` removido no 3.8)
- Detecta NICs ConnectX-5 automaticamente
- Gera `/etc/trex_cfg.yaml`
- Cria perfis de tráfego em `~/trex/profiles/`
- Cria helpers `run_trex` e `collect_baseline`

```bash
sudo ./setup_trex.sh [--trex-version VER] [--port0 BDF] [--port1 BDF]
```

**Perfis de tráfego criados:**
| Arquivo | Descrição |
|---|---|
| `steady_64b.py` | Fluxo contínuo, pacotes de 64 bytes |
| `steady_1500b.py` | Fluxo contínuo, pacotes de 1500 bytes (MTU padrão) |
| `bursty_64b.py` | Tráfego bursty configurável (revela o trade-off Leaky DMA) |

---

## Próximos passos

### Fase 1 — Baseline DPDK (imediato)

1. **Finalizar Servidor 2:** aguardar `setup_dpdk.sh` → reboot → rodar `setup_trex.sh`
2. **Testar conectividade back-to-back:**
   ```bash
   # Servidor 1
   ip link show  # interfaces mlx5 devem aparecer como UP
   ```
3. **Iniciar l2fwd no Servidor 1:**
   ```bash
   sudo ~/dpdk/build/examples/dpdk-l2fwd \
       -l 0-15 -n 4 \
       -a 0000:41:00.0 -a 0000:41:00.1 \
       -- -p 0x3 -T 1
   ```
4. **Teste de fumaça (Servidor 2):**
   ```bash
   run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30 --port 0 1
   ```
5. **Coleta do baseline completo (Servidor 2):**
   ```bash
   collect_baseline ~/trex/profiles/steady_64b.py   60 100% 10
   collect_baseline ~/trex/profiles/steady_1500b.py 60 100% 10
   collect_baseline ~/trex/profiles/bursty_64b.py   60 default 10
   ```
6. **Métricas adicionais (Servidor 1, durante os testes):**
   - Cache hit rate: `perf stat -e LLC-loads,LLC-load-misses -p <PID do l2fwd>`
   - Cycles/packet: contadores embutidos no l2fwd (flag `-T 1`)

### Fase 2 — Shring

- Clonar `https://github.com/BorisPis/shRing-dpdk` no Servidor 1
- Compilar substituindo o l2fwd
- Repetir os mesmos experimentos com os mesmos perfis T-Rex

### Fase 3 — Implementação própria

- Desenvolver os anéis duplos dinâmicos sobre DPDK/AF_XDP
- Repetir os mesmos experimentos
- Comparar as três fases

---

## Problemas encontrados e soluções

| Problema | Causa | Solução |
|---|---|---|
| `ninja: error: opening build log: Permission denied` | Build original foi feito com `sudo`, arquivos pertencem ao root | Usar `sudo ninja` para todos os builds |
| `l2fwd` não compilado após build do DPDK | Exemplos não são incluídos por padrão | `meson setup --reconfigure -Dexamples=l2fwd` |
| `EAL: Cannot create lock on /var/run/dpdk/rte/config` | Lock file de processo anterior ainda presente | `sudo rm -rf /var/run/dpdk/rte/` |
| `setup_trex.sh`: `isg` com unidade errada | Código passava nanosegundos; T-Rex espera microsegundos | Corrigido para `isg=args.idle_us` |
| `setup_trex.sh`: `run_trex` misturava `-i` com flags de batch | `-i` inicia modo servidor, incompatível com `-f`/`-m`/`-d` | Removido `-i` do helper |
| `setup_trex.sh`: `--output-file` inexistente no T-Rex CLI | Flag não existe — saída vai para stdout | Substituído por redirecionamento `> run.txt` |
