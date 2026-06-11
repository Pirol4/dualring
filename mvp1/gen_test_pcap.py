#!/usr/bin/env python3
"""Gera in.pcap (5000 UDP 64B) e empty.pcap para os testes locais do dual_ring_fwd."""
from scapy.all import Ether, IP, UDP, Raw, wrpcap

base = Ether(src='10:00:00:00:00:01', dst='ff:ff:ff:ff:ff:ff') / \
       IP(src='16.0.0.1', dst='48.0.0.1', ttl=64) / UDP(sport=1025, dport=12)
pad = b'\x00' * max(0, 60 - len(base))
wrpcap('in.pcap', [base / Raw(pad) for _ in range(5000)])
wrpcap('empty.pcap', [])
print("in.pcap (5000 pkts) e empty.pcap gerados.")
