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
        # Generate one block to an address
        address = node.getnewaddress()
        node.generatetoaddress(1, address)
        hash = node.getbestblockhash()
        txid = node.getblock(hash)["tx"][0]

        # Coinbase transaction is immature after 1 confirmation
        self.assert_category(node, "immature", address, txid, 0)

        # Mine another 99 blocks on top
        node.generate(99)
        # Coinbase transaction is still immature after 100 confirmations
        self.assert_category(node, "immature", address, txid, 99)

        # Mine one more block
        node.generate(1)
        # Coinbase transaction is now matured, so category is "generate"
        self.assert_category(node, "generate", address, txid, 100)

        # Orphan block that paid to address
        node.invalidateblock(hash)
        # Coinbase transaction is now orphaned
        self.assert_category(node, "orphan", address, txid, 100)

if __name__ == '__main__':
    CoinbaseCategoryTest().main()
