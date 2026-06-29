"""
Perfil: rajada SUSTENTAVEL de 64 bytes — o regime intermediario onde o
caminho de transbordo do DualRing de fato REDUZ a perda.

CONTEXTO
--------
Os dois perfis originais (steady_64b e bursty_64b) representam extremos:
  - steady_64b  : taxa baixa, sem perda em nenhum sistema.
  - bursty_64b  : 100% line-rate sustentado, perda satura em ~76% para TODOS
                  (a oferta media excede a capacidade -> nenhum buffer resolve).
Em ambos, o caminho de transbordo raramente muda a PERDA — so a latencia de
cauda. Este perfil cobre a lacuna: rajadas cuja MEDIA cabe na capacidade do
nucleo, mas cujo PICO instantaneo excede a taxa com que o privRing consegue
processar e encaminhar (que, sob o WorkPackage, e baixa por causa dos LLC
misses). Esse e o "joelho" citado como trabalho futuro no relatorio.

MECANISMO QUE ESTE PERFIL EXERCITA
----------------------------------
Sob o WorkPackage (--work-per-pkt 4 --work-mem-mb 80), encaminhar um pacote
custa ~varias centenas de ns (4 LLC misses): capacidade de forward C ~ 2-3 Mpps
por nucleo. Estacionar um pacote (memcpy de 64B + free) custa ~50-100 ns:
taxa de spill S ~ 10-15 Mpps. Logo, escolhendo

        C  <  burst_pps  <  S            (ex.: 3 Mpps < 8 Mpps < 15 Mpps)

  - privRing: dreno limitado a C; o anel RX raso (128) enche e a NIC descarta
              quase toda a rajada -> PERDA ALTA.
  - DualRing: em regime agudo faz so spill (a S >= burst_pps), a NIC nao
              descarta; a rajada inteira e absorvida no burst_ring (16384) e
              drenada (a C) no vale entre rajadas -> PERDA ~0.

CONDICOES PARA O REGIME INTERMEDIARIO (mantidas pelos defaults)
---------------------------------------------------------------
  (1) burst_pkts <= 16384            -> a rajada cabe no burst_ring.
  (2) burst_pps  <  taxa de spill S  -> o spill acompanha; a NIC nao descarta.
  (3) burst_pps  >  capacidade C     -> o privRing descarta (senao nao ha o que
                                        demonstrar).
  (4) media = burst_pkts/(burst_pkts/burst_pps + idle_us) < C
                                     -> o anel drena no vale; DualRing -> ~0.

Defaults: burst_pkts=8192, burst_pps=8e6, idle_us=4000
  duracao da rajada = 8192/8e6   = 1024 us
  periodo           = 1024 + 4000 = 5024 us
  taxa media        = 8192/5024us = 1.63 Mpps   (abaixo de C ~ 2-3 Mpps -> OK)

CALIBRACAO
----------
A capacidade C depende do hardware e do WorkPackage. Se a perda do DualRing
nao for ~0, AUMENTE idle_us (reduz a media) ate o anel drenar entre rajadas.
Se a perda do privRing for baixa demais para contrastar, AUMENTE burst_pps
ou burst_pkts. Acompanhe pelo contador [DR] backlog do l2fwd_dr: o pico deve
ficar perto de burst_pkts e voltar a ~0 antes da rajada seguinte.

Tunables (via -t no console: start -f ... -t burst_pkts=8192,idle_us=4000):
  burst_pkts   Pacotes por burst                 (padrao: 8192)
  burst_pps    Taxa durante o burst (pps)        (padrao: 8e6 = 8 Mpps)
  idle_us      Pausa entre bursts (microsegundos)(padrao: 4000)

Uso (console):  start -f profiles/bursty_sustain_64b.py -m 1 -d 60 -p 0 1
Uso (helper):   run_trex -f ~/trex/profiles/bursty_sustain_64b.py -m 1 -d 60
"""
from trex_stl_lib.api import *


class STLBurstySustain64(object):

    def get_streams(self, direction=0, burst_pkts=8192, burst_pps=8000000.0,
                    idle_us=4000.0, **kwargs):
        burst_pkts = int(burst_pkts)
        burst_pps = float(burst_pps)
        idle_us = float(idle_us)

        base_pkt = (Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') /
                    IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) /
                    UDP(sport=1025, dport=12))
        pad_len = max(0, 64 - len(base_pkt) - 4)
        pkt = base_pkt / Raw(b'\x00' * pad_len)

        # Rajada periodica: pkts_per_burst a 'pps', separadas por 'ibg' us.
        # count alto -> a duracao real e limitada pelo -d da sessao.
        burst_stream = STLStream(
            name='burst',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXMultiBurst(
                pps=burst_pps,
                pkts_per_burst=burst_pkts,
                ibg=idle_us,
                count=1000000,
            ),
            flow_stats=STLFlowStats(pg_id=5),
        )

        # Fluxo de latencia continuo e ralo (nao perturba a carga; mede RTT).
        lat_stream = STLStream(
            name='latency',
            packet=STLPktBuilder(pkt=pkt),
            mode=STLTXCont(pps=1000),
            flow_stats=STLFlowLatencyStats(pg_id=15),
        )

        return [burst_stream, lat_stream]


def register():
    return STLBurstySustain64()
