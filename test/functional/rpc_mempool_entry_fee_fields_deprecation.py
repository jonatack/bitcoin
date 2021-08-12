#!/usr/bin/env python3
# Copyright (c) 2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test deprecation of fee fields from top level mempool entry object"""

from test_framework.blocktools import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


def assertions_helper(new_object, deprecated_object, deprecated_fields):
    for field in deprecated_fields:
        assert field in deprecated_object
        assert field not in new_object


class MempoolFeeFieldsDeprecationTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [[], ["-deprecatedrpc=fees"]]

    def run_test(self):
        # we get spendable outputs from the premined chain starting
        # at block 76. see BitcoinTestFramework._initialize_chain() for details
        self.wallet = MiniWallet(self.nodes[0])
        self.wallet.scan_blocks(start=76, num=1)

        # we create the tx on the first node and wait until it syncs to node_deprecated
        # thus, any differences must be coming from getmempoolentry or getrawmempool
        txid = self.wallet.send_self_transfer(from_node=self.nodes[0])["txid"]
        self.sync_mempools(nodes=self.nodes)

        deprecated_fields = ["ancestorfees", "descendantfees", "modifiedfee", "fee"]
        self.test_getmempoolentry(txid, deprecated_fields)
        self.test_getrawmempool(txid, deprecated_fields)
        self.test_deprecated_fields_match(txid)

    def test_getmempoolentry(self, txid, deprecated_fields):

        self.log.info("Test getmempoolentry rpc")
        entry = self.nodes[0].getmempoolentry(txid)
        deprecated_entry = self.nodes[1].getmempoolentry(txid)
        assertions_helper(entry, deprecated_entry, deprecated_fields)

    def test_getrawmempool(self, txid, deprecated_fields):

        self.log.info("Test getrawmempool rpc")
        entry = self.nodes[0].getrawmempool(verbose=True)[txid]
        deprecated_entry = self.nodes[1].getrawmempool(verbose=True)[txid]
        assertions_helper(entry, deprecated_entry, deprecated_fields)

    def test_deprecated_fields_match(self, txid):

        self.log.info("Test deprecated fee fields match new fees object")
        entry = self.nodes[0].getmempoolentry(txid)
        deprecated_entry = self.nodes[1].getmempoolentry(txid)

        assert_equal(deprecated_entry["fee"], entry["fees"]["base"])
        assert_equal(deprecated_entry["modifiedfee"], entry["fees"]["modified"])
        assert_equal(deprecated_entry["descendantfees"], entry["fees"]["descendant"] * COIN)
        assert_equal(deprecated_entry["ancestorfees"], entry["fees"]["ancestor"] * COIN)


if __name__ == "__main__":
    MempoolFeeFieldsDeprecationTest().main()
