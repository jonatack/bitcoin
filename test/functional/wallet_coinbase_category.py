#!/usr/bin/env python3
# Copyright (c) 2014-2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test coinbase transactions return the correct categories.

Tests listtransactions, listsinceblock, and gettransaction.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_array_result
)

class CoinbaseCategoryTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_category(self, node, category, address, txid, skip):
        assert_array_result(node.listtransactions(skip=skip),
                            {"address": address},
                            {"category": category})
        assert_array_result(node.listsinceblock()["transactions"],
                            {"address": address},
                            {"category": category})
        assert_array_result(node.gettransaction(txid)["details"],
                            {"address": address},
                            {"category": category})

    def run_test(self):
        node = self.nodes[0]
        self.log.debug("Generate one block to an address")
        address = node.getnewaddress()
        node.generatetoaddress(1, address)
        hash = node.getbestblockhash()
        txid = node.getblock(hash)["tx"][0]

        self.log.info("Test coinbase txn is immature after 1 confirmation")
        self.assert_category(node, category="immature", address=address, txid=txid, skip=0)

        self.log.debug("Mine another 99 blocks on top")
        node.generate(99)
        self.log.info("Test coinbase txn is still immature after 100 confirmations")
        self.assert_category(node, category="immature", address=address, txid=txid, skip=99)

        self.log.debug("Mine one more block")
        node.generate(1)
        self.log.info("Test coinbase txn is mature and category is \"generate\" after 101 confirmations")
        self.assert_category(node, category="generate", address=address, txid=txid, skip=100)

        self.log.debug("Orphan block that paid to address")
        node.invalidateblock(hash)
        self.log.info("Test coinbase txn is orphaned after invalid block")
        self.assert_category(node, category="orphan", address=address, txid=txid, skip=100)

if __name__ == '__main__':
    CoinbaseCategoryTest().main()
