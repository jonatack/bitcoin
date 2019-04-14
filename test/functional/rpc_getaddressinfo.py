#!/usr/bin/env python3
# Copyright (c) 2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test RPC getaddressinfo.

Basic getaddressinfo tests/sanity checks not tested elsewhere.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error

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

        # Test invoking getaddressinfo with various invalid inputs.

        self.log.info('Test getaddressinfo with no address')
        assert_raises_rpc_error(-1, 'getaddressinfo "address"',
                                self.nodes[0].getaddressinfo)

        self.log.info('Test getaddressinfo with invalid address')
        assert_raises_rpc_error(-5, 'Invalid address',
                                self.nodes[0].getaddressinfo, 'invalid')

        self.log.info('Test getaddressinfo with array of valid addresses')
        assert_raises_rpc_error(-1, 'JSON value is not a string as expected',
                                self.nodes[0].getaddressinfo, [addr, addr])

        self.log.info('Test getaddressinfo with hash containing valid address')
        assert_raises_rpc_error(-1, 'JSON value is not a string as expected',
                                self.nodes[0].getaddressinfo, {'address': addr})

        # Test that getaddressinfo no longer returns an "addresses" array.

        self.log.info('Test getaddressinfo does not return "addresses" array')
        self.assert_no_addresses_array(addr)

        self.log.info('Test getaddressinfo with -deprecatedrpc=validateaddress'
                      ' does not return "addresses" array')
        self.restart_node(0, ['-deprecatedrpc=validateaddress'])
        self.assert_no_addresses_array(addr)

if __name__ == '__main__':
    RpcGetAddressInfoTest().main()
