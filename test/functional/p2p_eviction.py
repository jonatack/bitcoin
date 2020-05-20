#!/usr/bin/env python3
# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
""" Test node eviction logic

When the number of peers has reached the limit of maximum connections,
the next connecting inbound peer will trigger the eviction mechanism.
We cannot currently test the parts of the eviction logic that are based on
address/netgroup since in the current framework, all peers are connecting from
the same local address. See Issue #14210 for more info.

Therefore, this test is limited to the remaining protection criteria:
1.) Protect 4 nodes having sent us a block most recently
2.) Protect 4 nodes having sent us a transaction most recently
3.) Protect 8 nodes with the smallest minimum ping
"""

import time

from test_framework.test_framework import BitcoinTestFramework
from test_framework.mininode import P2PInterface, P2PDataStore
from test_framework.util import assert_equal, wait_until
from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTransaction, FromHex, msg_pong, msg_tx

class SlowP2PDataStore(P2PDataStore):
    def on_ping(self, message):
        time.sleep(0.1)
        self.send_message(msg_pong(message.nonce))

class SlowP2PInterface(P2PInterface):
    def on_ping(self, message):
        time.sleep(0.1)
        self.send_message(msg_pong(message.nonce))

class P2PEvict(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 1
        # The choice of 32 maxconnections results in a maximum of 21 inbound connections,
        # e.g. 32 conns - 10 outbound - 1 feeler) = 20 inbound peers protected from eviction.
        self.extra_args = [['-maxconnections=32']]

    def setup_network(self):
        self.setup_nodes()

    def setup_test_peers(self):
        self.protected_peers = set()
        node = self.nodes[0]
        node.generatetoaddress(101, node.get_deterministic_priv_key().address)

        # The first 4 peers send us a block, protecting them from eviction
        for _ in range(4):
            block_peer = node.add_p2p_connection(SlowP2PDataStore())
            block_peer.sync_with_ping(timeout=5)
            best_block = node.getbestblockhash()
            tip = int(best_block, 16)
            best_block_time = node.getblock(best_block)['time']
            block = create_block(tip, create_coinbase(node.getblockcount() + 1), best_block_time + 1)
            block.solve()
            block_peer.send_blocks_and_test([block], node, success=True)
            self.protected_peers.add(len(node.p2ps))

        # The next 5 nodes are slow-pinging peers, making them eviction candidates
        for _ in range(5):
            node.add_p2p_connection(SlowP2PInterface())

        # The next 4 peers send us a tx, protecting them from eviction
        for i in range(4):
            txpeer= node.add_p2p_connection(SlowP2PInterface())
            txpeer.sync_with_ping(timeout=5)

            prevtx = node.getblock(node.getblockhash(i+1), 2)['tx'][0]
            rawtx = node.createrawtransaction(
                inputs=[{'txid': prevtx['txid'], 'vout': 0}],
                outputs=[{node.get_deterministic_priv_key().address: 50 - 0.00125}],
            )
            sigtx = node.signrawtransactionwithkey(
                hexstring=rawtx,
                privkeys=[node.get_deterministic_priv_key().key],
                prevtxs=[{
                    'txid': prevtx['txid'],
                    'vout': 0,
                    'scriptPubKey': prevtx['vout'][0]['scriptPubKey']['hex'],
                }],
            )['hex']
            txpeer.send_message(msg_tx(FromHex(CTransaction(), sigtx)))
            self.protected_peers.add(len(node.p2ps))

        # The next 8 peers have faster pings, which will usually protect them from eviction
        for _ in range(8):
            fastpeer = node.add_p2p_connection(P2PInterface())
            wait_until(lambda: "ping" in fastpeer.last_message, timeout=10)

        # Make sure by asking the node what the actual min pings are
        peerinfo = node.getpeerinfo()
        pings = [[0 for __ in range(2)] for _ in range(len(node.p2ps))]

        # After adding 21 peers, the next one hits the maxconnection limit and triggers the eviction mechanism
        node.add_p2p_connection(SlowP2PInterface())

        # Usually the 8 fast peers are protected. In rare case of unreliable pings,
        # one of the slower peers might have a faster min ping though.
        for i in range(len(peerinfo)):
            pings[i][0] = i
            pings[i][1] = peerinfo[i]['minping'] if 'minping' in peerinfo[i] else 1000000

        sorted_pings = sorted(pings, key = lambda x:x[1])
        for i in range(8):
            self.protected_peers.add(sorted_pings[i][0])

        # One of the non-protected peers must be evicted. We can't be sure which one because
        # 4 peers are protected via netgroup, which is identical for all peers,
        # and the eviction mechanism doesn't preserve the order of identical elements.
        self.evicted_peers = []
        for i in range(len(node.p2ps)):
            if(node.p2ps[i].is_connected == False ) :
                self.evicted_peers.append(i)

    def run_test(self):
        self.log.info("Set up test peer network")
        self.setup_test_peers()

        self.log.info("Test that only one node was evicted")
        self.log.debug("{} evicted peers: {}".format(len(self.evicted_peers), set(self.evicted_peers)))
        assert_equal(len(self.evicted_peers), 1)

        self.log.info("Test that the evicted node is no longer in the set of protected nodes")
        self.log.debug("{} protected peers: {}".format(len(self.protected_peers), self.protected_peers))
        assert self.evicted_peers[0] not in self.protected_peers


if __name__ == '__main__':
    P2PEvict().main()
