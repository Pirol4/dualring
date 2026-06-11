"""Dois nós d6515 com dois enlaces de 100G (ConnectX-5) para benchmarking DPDK/T-Rex."""
import geni.portal as portal
import geni.rspec.pg as pg

pc = portal.Context()
request = pc.makeRequestRSpec()

UBUNTU20 = "urn:publicid:IDN+emulab.net+image+emulab-ops//UBUNTU20-64-STD"

server = request.RawPC("server")
server.hardware_type = "d6515"
server.disk_image = UBUNTU20

client = request.RawPC("client")
client.hardware_type = "d6515"
client.disk_image = UBUNTU20

# Dois links ponta a ponta a 100G (bandwidth em Kbps: 100G = 100.000.000)
for i in range(2):
    link = request.Link(f"link{i}")
    link.bandwidth = 100000000
    link.addInterface(server.addInterface(f"s_if{i}"))
    link.addInterface(client.addInterface(f"c_if{i}"))
    link.best_effort = True        # evita rejeição por reserva de banda na malha
    link.setNoBandwidthShaping()   # sem shaping — queremos line rate cru

pc.printRequestRSpec(request)
