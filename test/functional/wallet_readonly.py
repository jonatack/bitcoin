#!/usr/bin/env python3
# Copyright (c) 2017-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that getwalletinfo modifies the wallet file.

This is for the moment an inverse regression test to attempt to reproduce
https://github.com/bitcoin/bitcoin/issues/15608.

I'm unsure how to test the command `src/bitcoin-wallet info` in David's issue.
For now I'm attempting to reproduce the issue with an RPC getwalletinfo call.
"""

import os.path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_greater_than

class ReadonlyWalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def get_unix_timestamp_for_file(self, file_path, op):
        timestamp = os.path.getmtime(file_path)
        self.log.info(
            'Wallet file timestamp {0} getwalletinfo: {1}'.format(op, timestamp)
        )
        return timestamp

    def run_test(self):
        node = self.nodes[0]
        wallet = os.path.join(node.datadir, "regtest", "wallets", "wallet.dat")
        node.generate(1)

        self.log.info('Fetch wallet file timestamp before getwalletinfo')
        file_timestamp_before = self.get_unix_timestamp_for_file(wallet, 'before')

        self.log.info('Call getwalletinfo')
        node.getwalletinfo()

        self.log.info('Fetch wallet file timestamp after getwalletinfo')
        file_timestamp_after = self.get_unix_timestamp_for_file(wallet, 'after')

        self.log.info('Assert wallet file timestamp increased')
        assert_greater_than(file_timestamp_after, file_timestamp_before)

if __name__ == '__main__':
    ReadonlyWalletTest().main()
