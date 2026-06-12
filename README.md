# DualRing — Alocação Dinâmica de Buffers na LLC sobre DPDK

TCC de Mestrado (PPGCC/UFMG) — implementação de um sistema de anéis duplos de
recepção que elimina o trade-off entre absorção de micro-bursts e eficiência de
cache em redes de alta velocidade.

---

## O problema: Leaky DMA

NICs modernas escrevem pacotes diretamente na LLC via Intel DDIO (ou equivalente
AMD). Isso é eficiente quando o anel de RX é pequeno — o working set fica quente
na cache. Mas rajadas de tráfego forçam anéis grandes, que expulsam dados úteis
da LLC. Este é o **Leaky DMA**: anéis grandes absorvem bursts mas "vazam" cache.

A solução do estado da arte (ShRing) usa anéis compartilhados de tamanho fixo.
Este projeto propõe uma alternativa dinâmica, em software puro, sem modificar
firmware ou hardware.

---

## A solução: Dual Receive Ring

Dois mempools DPDK com papéis distintos:

- **Fast pool** — pequeno (~4 MiB), dimensionado para residir num CCX da LLC do
  AMD EPYC 7402P. Alimenta a fila de RX da NIC; reciclagem rápida mantém o
  working set quente (DDIO efetivo).
- **Burst pool** — grande (~140 MiB, DRAM). Sob pressão no fast pool (rajada de
  tráfego), pacotes são copiados para o burst pool e os buffers fast liberados
  imediatamente — mantendo o working set da NIC pequeno mesmo durante bursts.

O monitoramento de `LLC-load-misses` via `perf_event_open` guia a redistribuição
dinâmica entre os dois pools (MVP 2).

---

## Estrutura do repositório

```
mvp1/
  dual_ring_fwd.c     encaminhador L2 com dual rings (contribuição central)
  Makefile
  gen_test_pcap.py    gera pcaps para teste local sem hardware
  README.md           instruções de build, execução e validação

setup.sh              setup DPDK + hugepages nos nós CloudLab (ambos)
setup_trex.sh         instala e configura T-Rex no nó gerador
profile.py            topologia CloudLab: 2× d6515 + 2 enlaces 100G

results/              dados experimentais (CSV por configuração)
```

---

## Ambiente

Dois nós **AMD EPYC d6515** no CloudLab Utah, conectados back-to-back por
2× NICs NVIDIA/Mellanox ConnectX-5 Ex de 100 GbE.

| Nó | Papel |
|---|---|
| Servidor 1 (DUT) | Roda o dual_ring_fwd |
| Servidor 2 (client) | Gerador de carga T-Rex v3.06 |

Software: Ubuntu 20.04, DPDK 22.11 LTS, kernel 5.4.0.

---

## Rodando o MVP 1

### 1. Setup dos nós (ambos)

```bash
sudo bash setup.sh
sudo reboot
```

### 2. Setup do gerador (Servidor 2)

```bash
sudo bash setup_trex.sh
```

### 3. Baseline l2fwd (Servidor 1)

```bash
sudo ~/dpdk/build/examples/dpdk-l2fwd \
    -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -p 0x3 -T 1
```

### 4. Compilar e rodar o dual_ring_fwd (Servidor 1)

```bash
cd mvp1/
export PKG_CONFIG_PATH=$(find ~/dpdk/build -name 'libdpdk.pc' -exec dirname {} \; | head -1)
make
sudo ./dual_ring_fwd -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -T 1
```

Flags úteis:

```
--fast-mbufs N        tamanho do fast pool (padrão 2048)
--burst-mbufs N       tamanho do burst pool (padrão 65536)
--spill-watermark N   gatilho de spill: fast avail < N (padrão 256)
--force-spill         força o caminho de spill (debug)
```

### 5. Gerar tráfego (Servidor 2)

```bash
# Tiro curto de validação
run_trex -f ~/trex/profiles/steady_64b.py -m 10% -d 30

# Coleta de baseline completa (5 repetições)
collect_baseline ~/trex/profiles/bursty_64b.py 60 100% 5
```

### Teste local sem hardware (vdev pcap)

```bash
cd mvp1/
python3 gen_test_pcap.py
# Caminho rápido
timeout -s INT 8 ./dual_ring_fwd --no-huge -m 700 -l 0 \
    --vdev "net_pcap0,rx_pcap=in.pcap,tx_pcap=out0.pcap" \
    --vdev "net_pcap1,rx_pcap=empty.pcap,tx_pcap=out1.pcap" --
```

---

## Coletando experimentos (workflow completo)

A coleta usa dois scripts sincronizados por um `exp_id` — um timestamp gerado no servidor que
garante que os dados de rede (client) e LLC (server) ficam na mesma pasta.

Você precisa de **3 terminais abertos ao mesmo tempo**:

### Terminal 1 — Servidor (rodar a implementação)

```bash
# Exemplo: baseline l2fwd
sudo ~/dpdk/build/examples/dpdk-l2fwd \
    -l 0-15 -n 4 \
    -a 0000:41:00.0 -a 0000:41:00.1 \
    -- -p 0x3 -T 1

# Exemplo: dual_ring_fwd (MVP 1)
cd ~/POC_DualRingProject/mvp1
export PKG_CONFIG_PATH=$(find ~/dpdk/build -name 'libdpdk.pc' -exec dirname {} \; | head -1)
make
sudo ./dual_ring_fwd -l 0-15 -n 4 -a 0000:41:00.0 -a 0000:41:00.1 -- -T 1
```

Deixe rodando. Não feche este terminal.

### Terminal 2 — Servidor (monitor LLC)

```bash
# Instale perf se ainda não estiver (só precisa uma vez por alocação CloudLab)
sudo apt-get install -y linux-tools-$(uname -r) linux-tools-generic

# Gere o exp_id e anote o valor
exp_id=$(date +%Y%m%d_%H%M%S)
echo "EXP_ID: $exp_id"

# Inicie o monitor — fica aguardando
sudo bash ~/POC_DualRingProject/scripts/collect_server.sh l2fwd $exp_id
```

Quando aparecer `>>> INICIE o collect_client.sh no gerador AGORA <<<`, vá para o Terminal 3.

### Terminal 3 — Client/gerador (T-Rex)

```bash
# Substitua o exp_id pelo valor exato do Terminal 2
exp_id=20260611_174501

mkdir -p ~/results
bash ~/POC_DualRingProject/scripts/collect_client.sh l2fwd $exp_id
```

A coleta dura ~12 minutos (2 perfis × 5 reps × 60 s + intervalos). Ao terminar, o Terminal 2
também encerra automaticamente. Encerre o Terminal 1 com `Ctrl+C`.

**Parâmetros fixos** (não altere entre implementações para garantir comparabilidade):
- `steady_64b` @ 3% line rate — verifica operação normal sem perda
- `bursty_64b` @ multiplicador 1 — expõe o trade-off Leaky DMA
- 5 repetições, 60 s cada, 10 s de intervalo entre reps

### Salvando os resultados (antes de encerrar o CloudLab)

As máquinas CloudLab são efêmeras. Salve os resultados antes de encerrar o experimento.

**Opção 1 — git push** (recomendado, mantém histórico):
```bash
# No servidor (e também no client — rode em ambos)
cd ~/POC_DualRingProject
git add results/
git commit -m "results: l2fwd 20260611_174501"

# Configure credenciais se necessário (token do GitHub):
git remote set-url origin https://<TOKEN>@github.com/Pirol4/POC_DualRingProject.git
git push
```

**Opção 2 — scp para máquina local** (do seu computador):
```bash
# Baixa todos os resultados do servidor para ~/Downloads/results_server/
scp -r -P 22 pirola@server.pirola-307281.aos-ufmg-dcc831-pg0.utah.cloudlab.us:~/results/ \
    ~/Downloads/results_server/

# Baixa todos os resultados do client para ~/Downloads/results_client/
scp -r -P 22 pirola@<IP_DO_CLIENT>:~/results/ ~/Downloads/results_client/
```

> Os arquivos `.jsonl` (dados brutos de cada run) estão no `.gitignore`. Para commitar os dados
> brutos também, use `git add -f results/` no lugar de `git add results/`.

---

## Sistemas comparados

| Sistema | Descrição |
|---|---|
| **l2fwd** (baseline) | DPDK padrão, anel único privado por core |
| **ShRing** | Anel compartilhado de tamanho fixo (estado da arte) |
| **DualRing** (este trabalho) | Fast pool LLC-residente + burst pool DRAM, redistribuição dinâmica |

---

## Métricas coletadas

| Métrica | Ferramenta |
|---|---|
| Throughput (Mpps) e perda | T-Rex |
| LLC-load-misses | `perf_event_open` / `perf stat` |
| Cycles por pacote | Contadores de ciclo embutidos |
| Ocupação do burst ring | Contadores internos do dual_ring_fwd |
