# Contexto do TCC — DualRing Project

## O que é esse projeto?

Pesquisa de Conclusão de Curso (PPGCC/UFMG) que implementa um sistema de **alocação dinâmica de buffers na LLC (Last Level Cache)** usando uma estrutura de **anéis duplos de recepção** sobre DPDK, com monitoramento via eBPF/`perf_event_open`. O objetivo é eliminar o trade-off entre absorção de micro-bursts e eficiência de cache em redes de alta velocidade.

**Problema central — Leaky DMA:** NICs modernas fazem DMA diretamente na LLC via Intel DDIO (ou equivalente AMD). Anéis de recepção grandes absorvem rajadas mas expulsam dados úteis da LLC; anéis pequenos preservam a cache mas causam perda de pacotes em picos de tráfego. Este projeto resolve esse trade-off de forma **dinâmica, em software puro**, sem modificar firmware ou hardware.

**Solução proposta — Dual Receive Ring:**
- **Fast ring:** pequeno, LLC-residente, atende tráfego normal
- **Burst ring:** grande, DRAM-residente, ativado sob picos de tráfego
- **eBPF monitor:** usa `perf_event_open` para monitorar `LLC-load-misses` em tempo real e redistribuir buffers dinamicamente entre os dois anéis

---

## Repositório

- **GitHub:** https://github.com/Pirol4/POC_DualRingProject
- **Branch principal:** `main`
- **Linguagens:** C (aplicação DPDK), Shell (scripts de setup)
- **Estrutura:**
  ```
  src/           # código C: server.c, client.c, common.h
  setup.sh       # setup geral do sistema (DPDK, hugepages, GRUB)
  setup_trex.sh  # instala e configura T-Rex no gerador de carga
  claude_setup.sh # instala Claude Code nas máquinas CloudLab
  Makefile
  CLAUDE.md      # contexto resumido (versão no repo)
  progress.md    # log detalhado do que foi feito
  ```

---

## Ambiente de Desenvolvimento — CloudLab

### Máquinas

Dois nós **d6515** alocados no [CloudLab](https://www.cloudlab.us/), conectados **back-to-back** pelas NICs de alta velocidade.

| Servidor   | Hostname                                                          | Papel                  |
|------------|-------------------------------------------------------------------|------------------------|
| Servidor 1 | `server.pirola-307281.aos-ufmg-dcc831-pg0.utah.cloudlab.us`      | DUT (sistema avaliado) |
| Servidor 2 | (mesmo experimento CloudLab)                                      | Gerador de carga (T-Rex) |

> ⚠️ **Importante:** As máquinas CloudLab são efêmeras — o experimento expira e a alocação pode ser perdida. Todos os scripts de setup devem ser re-executados em cada nova alocação.

### Hardware real (d6515)

> O `CLAUDE.md` do repositório descreve o hardware do artigo Shring (referência). O hardware **real** das d6515 no CloudLab é diferente:

| Componente  | Valor                                                   |
|-------------|---------------------------------------------------------|
| CPU         | AMD EPYC 7452 — 32 cores físicos, 64 threads (HT ativo)|
| Frequência  | 3340 MHz                                                |
| L1d / L1i   | 1 MiB cada                                              |
| L2          | 16 MiB                                                  |
| L3 (LLC)    | 128 MiB                                                 |
| RAM         | 125 GiB                                                 |
| NUMA nodes  | 1                                                       |
| NICs        | 2× NVIDIA/Mellanox ConnectX-5 Ex (MT28800) — 100 GbE   |
| PCIe NICs   | `0000:41:00.0` e `0000:41:00.1`                         |
| OS          | Ubuntu 20.04 LTS (Focal Fossa)                          |
| Kernel      | Linux 5.4.0-100-generic                                 |

### Estado atual do Servidor 1 (após setup_dpdk.sh + reboot)

- **DPDK 22.11.0 LTS** compilado em `~/dpdk/build/`
- **Hugepages:** 8× 1 GiB, montadas em `/dev/hugepages`
- **Parâmetros GRUB ativos:** `isolcpus=1-63`, `hugepages=8`, `mitigations=off`, `amd_iommu=on`, `iommu=pt`
- **NICs em modo bifurcado:** driver `mlx5_core` (DPDK acessa via PMD mlx5 sem bind para vfio-pci)
- **Binários compilados:**
  - `~/dpdk/build/app/dpdk-testpmd`
  - `~/dpdk/build/examples/dpdk-l2fwd`
- **Validação:** `dpdk-testpmd` confirma PMD `mlx5_pci` carregado, MAC `1C:34:DA:41:CF:CC`

---

## Software e Baseline de Comparação

| Sistema          | Repositório / Referência                        | Status        |
|------------------|-------------------------------------------------|---------------|
| DPDK 22.11 LTS   | https://github.com/DPDK/dpdk                   | ✅ Compilado  |
| Shring           | https://github.com/BorisPis/shRing-dpdk        | Fase 2        |
| RxBisect         | (comparar depois de Shring)                    | Fase 3+       |
| **DualRing**     | Implementação própria (contribuição do TCC)    | Em desenvolvimento |

### Gerador de carga: T-Rex

- **Versão:** 3.06
- **Modo:** Stateless (`--stl`)
- **Localização no Servidor 2:** `~/trex/v3.06/`
- **Config:** `/etc/trex_cfg.yaml`
- **Perfis de tráfego em** `~/trex/profiles/`:

| Arquivo           | Descrição                                                    |
|-------------------|--------------------------------------------------------------|
| `steady_64b.py`   | Fluxo contínuo, pacotes de 64 bytes                          |
| `steady_1500b.py` | Fluxo contínuo, pacotes de 1500 bytes (MTU padrão)           |
| `bursty_64b.py`   | Tráfego bursty configurável (revela o trade-off Leaky DMA)   |

**Executar T-Rex (modo batch):**
```bash
cd ~/trex/v3.06
sudo ./t-rex-64 --stl -f ~/trex/profiles/steady_64b.py -m 100% -d 60 --port 0 1
```

---

## Configurações de Referência (artigo Shring)

> Usadas como referência metodológica, **não** como hardware real.

| Parâmetro              | Valor padrão           |
|------------------------|------------------------|
| Rx ring (descritores)  | 1024                   |
| Tx ring (descritores)  | 1024                   |
| DDIO LLC ways          | 2                      |
| CPU cores utilizados   | 16 (todos disponíveis) |
| Cores por NIC          | 8                      |

---

## Mecanismos de Ring Comparados

| Mecanismo        | Descrição                                                                                    |
|------------------|----------------------------------------------------------------------------------------------|
| **privRing**     | Ring privado por core (DPDK padrão)                                                          |
| **shRing/8**     | Array ring compartilhado (não dinâmico) entre 8 cores                                       |
| **small privRing** | privRing com descritores equivalentes ao shRing/8 (128/ring); causa perda em tráfego bursty |
| **DualRing**     | **Nossa contribuição** — dois anéis dinâmicos com redistribuição por eBPF                   |

---

## Metodologia de Medição

| Métrica                        | Ferramenta                                                    |
|--------------------------------|---------------------------------------------------------------|
| Cycles per packet              | Contadores de ciclo embutidos na aplicação                    |
| Cache hit rate / LLC-misses    | `perf stat -e LLC-loads,LLC-load-misses -p <PID>`             |
| Tx ring occupancy              | Comparação dos índices producer/consumer do completion ring   |
| PCIe latency                   | NVIDIA Mellanox Neo-host                                      |
| Memory bandwidth / PCIe hit    | Intel PCM (ou AMD equivalente)                                |

**Metodologia estatística:**
- 10 repetições por experimento
- Trimmed mean (descartando mínimo e máximo)
- Desvio padrão alvo: < 5%

---

## Plano de Desenvolvimento (Fases)

### ✅ Fase 0 — Infraestrutura (concluída)
- Setup DPDK 22.11 no Servidor 1
- NICs ConnectX-5 validadas
- Scripts `setup.sh` e `setup_trex.sh` funcionais

### 🔄 Fase 1 — Baseline DPDK l2fwd (em andamento)
1. Finalizar setup do Servidor 2 (setup_dpdk.sh → reboot → setup_trex.sh)
2. Validar link back-to-back
3. Iniciar l2fwd no Servidor 1:
   ```bash
   sudo ~/dpdk/build/examples/dpdk-l2fwd \
       -l 0-15 -n 4 \
       -a 0000:41:00.0 -a 0000:41:00.1 \
       -- -p 0x3 -T 1
   ```
4. Coletar baseline com T-Rex (steady_64b, steady_1500b, bursty_64b)
5. Coletar métricas de LLC com `perf stat`

### ⏳ Fase 2 — Shring
- Clonar e compilar `BorisPis/shRing-dpdk` (fork do DPDK 21.05)
- Repetir experimentos com mesmos perfis T-Rex

### ⏳ Fase 3 — DualRing (implementação própria)
- Desenvolver dual receive ring com monitor eBPF
- Integrar `perf_event_open` para monitorar LLC-load-misses
- Redistribuição dinâmica de buffers entre fast ring e burst ring
- Repetir experimentos e comparar com Fases 1 e 2

---

## Problemas Conhecidos e Soluções

| Problema                                                          | Causa                                                        | Solução                                             |
|-------------------------------------------------------------------|--------------------------------------------------------------|-----------------------------------------------------|
| `ninja: error: opening build log: Permission denied`             | Build feito com sudo, arquivos pertencem a root              | Usar `sudo ninja` para todos os builds              |
| `l2fwd` não compilado após build DPDK                            | Exemplos não incluídos por padrão                            | `meson setup --reconfigure -Dexamples=l2fwd`        |
| `EAL: Cannot create lock on /var/run/dpdk/rte/config`            | Lock file de processo anterior                               | `sudo rm -rf /var/run/dpdk/rte/`                    |
| `setup_trex.sh`: `isg` com unidade errada                        | Código passava ns; T-Rex espera µs                           | Corrigido para `isg=args.idle_us`                   |
| `run_trex` misturava `-i` com flags de batch                     | `-i` inicia modo servidor, incompatível com `-f`/`-m`/`-d`  | Removido `-i` do helper                             |
| `--output-file` inexistente no T-Rex CLI                         | Flag não existe — saída vai para stdout                      | Substituído por redirecionamento `> run.txt`         |
| `run_trex` wrapper com PATH incorreto após patching              | Shell resolve binário diferente de `/usr/local/bin/run_trex` | Verificar com `which run_trex` e `type run_trex`    |
| `ibverbs-providers` ausente no Ubuntu 20.04 (Focal)              | Pacote não instalado por padrão                              | `apt install ibverbs-providers`                     |
| Grafana GPG key quebrada no apt sources                          | Key expirada/removida                                        | Usar repositório alternativo ou instalar manualmente|

---

## Comandos Úteis de Referência

```bash
# Verificar hugepages
cat /proc/meminfo | grep -i huge

# Verificar NICs DPDK
sudo dpdk-devbind.py --status

# Verificar parâmetros GRUB ativos
cat /proc/cmdline

# Monitorar LLC-misses durante experimento
sudo perf stat -e LLC-loads,LLC-load-misses,cache-misses -p $(pgrep dpdk-l2fwd) -I 1000

# Sessão tmux persistente (não perde com desconexão SSH)
tmux new -s tcc
tmux attach -t tcc

# SSH keepalive (no ~/.ssh/config do cliente)
# Host *.cloudlab.us
#   ServerAliveInterval 60
#   ServerAliveCountMax 10
```

---

## Observações para o Claude

- Este projeto é um TCC de mestrado (PPGCC) — foco em rigor científico e reprodutibilidade
- O hardware real (AMD EPYC d6515) difere do hardware de referência do artigo Shring (Intel Xeon Silver 4216) — isso **deve ser documentado e discutido** no TCC
- A arquitetura proposta (DualRing) usa eBPF/AF_XDP, que não estava disponível no artigo Shring original — isso é uma contribuição técnica relevante
- Ao ajudar com código C para DPDK, sempre considerar: versão 22.11 LTS, PMD mlx5, modo bifurcado, hugepages 1G
- Ao ajudar com scripts bash, sempre considerar: Ubuntu 20.04, Python 3.8 (sem `platform.dist`), ambiente CloudLab efêmero
