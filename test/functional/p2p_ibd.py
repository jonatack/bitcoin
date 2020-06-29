#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test p2p messages and filters during and after IBD."""

from decimal import Decimal

from test_framework.messages import COIN
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

MAX_FEE_FILTER = Decimal(str(9170997 / COIN))
NORMAL_FEE_FILTER = Decimal(str(100 / COIN))


class P2PIBDTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 2
        self.extra_args = [["-minrelaytxfee=0.00000100"], ["-minrelaytxfee=0.00000100"]]

    def run_test(self):
        self.log.info("Test in IBD no invs are sent and nodes set minfilter to MAX_MONEY")
        for node in self.nodes:
            assert_equal(node.getblockchaininfo()["initialblockdownload"], True)
            info = node.getpeerinfo()[0]
            assert_equal(info["minfeefilter"], MAX_FEE_FILTER)
            assert "inv" not in info["bytesrecv_per_msg"].keys()
            assert "inv" not in info["bytessent_per_msg"].keys()

        self.nodes[0].generate(1)  # Get out of IBD
        self.sync_all()

        self.log.info("Test after IBD invs are sent and nodes reset minfilter to normal")
        for node in self.nodes:
            assert_equal(node.getblockchaininfo()["initialblockdownload"], False)
            info = node.getpeerinfo()[0]
            assert_equal(info["minfeefilter"], NORMAL_FEE_FILTER)
            assert "inv" in info["bytesrecv_per_msg"].keys() or "inv" in info["bytessent_per_msg"].keys()


if __name__ == "__main__":
    P2PIBDTest().main()
