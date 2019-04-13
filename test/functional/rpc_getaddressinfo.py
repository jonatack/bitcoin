#!/usr/bin/env python3
# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test RPC getaddressinfo."""

from test_framework.test_framework import BitcoinTestFramework

class RpcGetAddressInfoTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def assert_no_addresses_array(self, addr):
        assert not 'addresses' in self.nodes[0].getaddressinfo(addr)

    def run_test(self):
        addr = self.nodes[0].getnewaddress()
        self.log.info('Test getaddressinfo does not return "addresses" array')
        self.assert_no_addresses_array(addr)
        self.log.info('Test getaddressinfo with -deprecatedrpc=validateaddress does not return "addresses" array')
        self.restart_node(0, ['-deprecatedrpc=validateaddress'])
        self.assert_no_addresses_array(addr)

if __name__ == '__main__':
    RpcGetAddressInfoTest().main()
