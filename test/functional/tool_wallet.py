#!/usr/bin/env python3
# Copyright (c) 2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test bitcoin-wallet."""
import subprocess
import textwrap

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

class ToolWalletTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def bitcoin_wallet_process(self, *args):
        binary = self.config["environment"]["BUILDDIR"] + '/src/bitcoin-wallet' + self.config["environment"]["EXEEXT"]
        args = ['-datadir={}'.format(self.nodes[0].datadir), '-regtest'] + list(args)
        return subprocess.Popen([binary] + args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)

    def assert_raises_tool_error(self, error, *args):
        p = self.bitcoin_wallet_process(*args)
        stdout, stderr = p.communicate()
        assert_equal(p.poll(), 1)
        assert_equal(stdout, '')
        assert_equal(stderr.strip(), error)

    def assert_tool_output(self, output, *args):
        p = self.bitcoin_wallet_process(*args)
        stdout, stderr = p.communicate()
        assert_equal(p.poll(), 0)
        assert_equal(stderr, '')
        assert_equal(stdout, output)

    def run_test(self):
        self.log.info('Test raising and error messages with various bad commands')
        self.assert_raises_tool_error('Invalid command: foo', 'foo')
        # `bitcoin-wallet help` is an error. Use `bitcoin-wallet -help`
        self.assert_raises_tool_error('Invalid command: help', 'help')
        self.assert_raises_tool_error('Error: two methods provided (info and create). Only one method should be provided.', 'info', 'create')
        self.assert_raises_tool_error('Error parsing command line arguments: Invalid parameter -foo', '-foo')
        self.assert_raises_tool_error('Error loading wallet.dat. Is wallet being used by other process?', '-wallet=wallet.dat', 'info')
        self.assert_raises_tool_error('Error: no wallet file at nonexistent.dat', '-wallet=nonexistent.dat', 'info')

        # Stop the node to close the wallet to call the info command.
        self.log.info('Stop node')
        self.stop_node(0)

        self.log.info('Call wallet tool info')
        out = textwrap.dedent('''\
            Wallet info
            ===========
            Encrypted: no
            HD (hd seed available): yes
            Keypool Size: 2
            Transactions: 0
            Address Book: 3
        ''')
        self.assert_tool_output(out, '-wallet=wallet.dat', 'info')

        # Mutate the wallet to check the info command output changes accordingly.
        self.log.info('Start node')
        self.start_node(0)

        self.log.info('Generate transaction to mutate wallet')
        self.nodes[0].generate(1)

        self.log.info('Stop node')
        self.stop_node(0)

        self.log.info('Call wallet tool info after generating a transaction')
        out = textwrap.dedent('''\
            Wallet info
            ===========
            Encrypted: no
            HD (hd seed available): yes
            Keypool Size: 2
            Transactions: 1
            Address Book: 3
        ''')
        self.assert_tool_output(out, '-wallet=wallet.dat', 'info')

        self.log.info('Call wallet tool create')
        out = textwrap.dedent('''\
            Topping up keypool...
            Wallet info
            ===========
            Encrypted: no
            HD (hd seed available): yes
            Keypool Size: 2000
            Transactions: 0
            Address Book: 0
        ''')
        self.assert_tool_output(out, '-wallet=foo', 'create')

        self.log.info('Start node with arg -wallet=foo')
        self.start_node(0, ['-wallet=foo'])

        self.log.info('Call getwalletinfo()')
        out = self.nodes[0].getwalletinfo()

        self.log.info('Stop node')
        self.stop_node(0)

        assert_equal(0, out['txcount'])
        assert_equal(1000, out['keypoolsize'])
        assert_equal(1000, out['keypoolsize_hd_internal'])
        assert_equal(True, 'hdseedid' in out)

if __name__ == '__main__':
    ToolWalletTest().main()
