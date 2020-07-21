#!/usr/bin/env python3
import os
import sys
import time

sys.path.append("./bitcoin/test/functional")
from test_framework.authproxy import AuthServiceProxy, JSONRPCException

datadir = os.getenv("DATADIR", os.path.join(os.getenv("HOME"), ".bitcoin"))

with open(os.path.join(datadir, ".cookie"), "r") as f:
    cookie = f.read()

target = "http://%s@127.0.0.1:8332/" % (cookie)
proxy = AuthServiceProxy(target)
out = sys.stdout  # stream to write to

while True:
    now = time.time()
    peers = proxy.getpeerinfo()
    in_ipv4 = 0
    in_ipv6 = 0
    in_tor = 0
    out_ipv4 = 0
    out_ipv6 = 0
    out_tor = 0
    print("Outbound and inbound peer connections")

    for peer in peers:
        addr = peer["addr"]
        addrbind = peer["addrbind"]

        if "minping" in peer.keys():
            minping = str(peer["minping"])[:4]
        else:
            minping = ""

        if "addrlocal" in peer.keys():
            addrlocal = peer["addrlocal"]
        else:
            addrlocal = ""

        if peer["relaytxes"]:
            relay = " full-relay"
        else:
            relay = "block-relay"

        if peer["inbound"]:
            if addr.startswith("["):
                print("in   {}, minping {}, addr {}".format(relay, minping, addr))
                in_ipv6 += 1
            elif addr.startswith("127.0.0.1:"): # and ".onion" in addrlocal and "mapped_as" not in peer.keys():
                print("in  *{}, minping*{}, addr {}, addrlocal {}".format(relay, minping, addr, addrlocal))
                in_tor += 1
            else:
                print("in   {}, minping {}, addr {}, addrlocal {}".format(relay, minping, addr, addrlocal))
                in_ipv4 += 1
        else:
            if addr.startswith("["):
                print("out  {}, minping {}, addr {}".format(relay, minping, addr))
                out_ipv6 += 1
            elif ".onion" in addr:
                print("out *{}, minping*{}, addr {}, addrbind {}".format(relay, minping, addr, addrbind))
                out_tor += 1
            else:
                print("out  {}, minping {}, addr {}, addrbind {}".format(relay, minping, addr, addrbind))
                out_ipv4 += 1
    print()
    print("in:  ipv4 {:>3}  |  ipv6 {:>3}   |  onion {:>3}".format(in_ipv4, in_ipv6, in_tor))
    print("out: ipv4 {:>3}  |  ipv6 {:>3}   |  onion {:>3}".format(out_ipv4, out_ipv6, out_tor))
    print("all: {}\n".format(proxy.getconnectioncount()))

    for addr in proxy.getnetworkinfo()["localaddresses"]:
        print("addr {:<37}  |  port {}  |  score {:>5}".format(addr["address"], addr["port"], addr["score"]))

    ni = proxy.getblockchaininfo()
    bi = proxy.getblock(ni["bestblockhash"])
    print("Last block was {} on {}".format(ni["blocks"], time.ctime(bi["time"])))
    break
