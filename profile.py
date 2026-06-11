# -*- coding: utf-8 -*-
"""Two d6515 nodes with two 100G links (ConnectX-5) for DPDK/T-Rex benchmarking."""
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

# Two point-to-point links at 100G (bandwidth in Kbps: 100G = 100000000)
for i in range(2):
    link = request.Link("link%d" % i)
    link.bandwidth = 100000000
    link.addInterface(server.addInterface("s_if%d" % i))
    link.addInterface(client.addInterface("c_if%d" % i))
    link.best_effort = True

pc.printRequestRSpec(request)
