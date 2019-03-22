#!/usr/bin/env python3
# Copyright (c) 2017-2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that the wallet resends transactions periodically."""
from collections import defaultdict
import time

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import ToHex
from test_framework.mininode import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until

class P2PStoreTxInvs(P2PInterface):
    def __init__(self):
        super().__init__()
        self.tx_invs_received = defaultdict(int)

    def on_inv(self, message):
        # Store how many times invs have been received for each tx.
        for i in message.inv:
            if i.type == 1:
                # save as hex
                self.tx_invs_received[format(i.hash, 'x')] += 1

class ResendWalletTransactionsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.nodes[0].add_p2p_connection(P2PStoreTxInvs())

        # Create a new transaction. Set time to 31 minutes in the past.
        time_now = int(time.time())
        self.nodes[0].setmocktime(time_now - 60 * 31)
        txid = self.nodes[0].sendtoaddress(self.nodes[0].getnewaddress(), 1)

        # Can take a few seconds due to transaction trickling
        wait_until(lambda: self.nodes[0].p2p.tx_invs_received[txid] >= 1)
        time_now = int(time.time())

        # Add a second peer since txs aren't rebroadcast to the same peer (see filterInventoryKnown)
        self.nodes[0].add_p2p_connection(P2PStoreTxInvs())

        # Mine a block. Transactions are only rebroadcast if a block has been mined
        # since the last time we tried to broadcast. Make sure that the transaction is
        # not included in the block.
        # The block must be received more than 5 minutes after the transaction timestamp for the
        # transaction to be rebroadcast.
        self.nodes[0].setmocktime(time_now - 60 * 25)
        block = create_block(int(self.nodes[0].getbestblockhash(), 16), create_coinbase(self.nodes[0].getblockchaininfo()['blocks']), time_now)
        block.nVersion = 3
        block.rehash()
        block.solve()
        self.nodes[0].submitblock(ToHex(block))

        # Transaction should not be rebroadcast
        self.nodes[0].p2ps[1].sync_with_ping()
        assert_equal(self.nodes[0].p2ps[1].tx_invs_received[txid], 0)

        # Transaction should be broadcast after 30 minutes. Give an extra minute to be sure
        self.nodes[0].setmocktime(0)
        wait_until(lambda: self.nodes[0].p2ps[1].tx_invs_received[txid] >= 1)

if __name__ == '__main__':
    ResendWalletTransactionsTest().main()
