#!/usr/bin/env python3
# Copyright (c) 2018 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test bitcoin-wallet."""

import hashlib
import os
import subprocess
import textwrap

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than

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

    def wallet_path(self):
        return os.path.join(self.nodes[0].datadir, "regtest", "wallets", "wallet.dat")

    def wallet_shasum(self):
        h = hashlib.sha1()
        ba = bytearray(128*1024)
        mv = memoryview(ba)
        with open(self.wallet_path(), 'rb', buffering=0) as f:
            for n in iter(lambda : f.readinto(mv), 0):
                h.update(mv[:n])
        return h.hexdigest()

    def wallet_timestamp(self, op):
        timestamp = os.path.getmtime(self.wallet_path())
        self.log.info('Wallet file timestamp {0}: {1}'.format(op, timestamp))
        return timestamp

    def log_wallet_timestamp_comparison(self, new, old):
        result = 'unchanged' if new == old else 'increased! **FIX ME**'
        self.log.info('Wallet file timestamp {0}\n'.format(result))

    def run_test(self):
        self.log.info('Test that various invalid commands raise with specific error messages')
        self.assert_raises_tool_error('Invalid command: foo', 'foo')
        # `bitcoin-wallet help` is an error. Use `bitcoin-wallet -help`
        self.assert_raises_tool_error('Invalid command: help', 'help')
        self.assert_raises_tool_error('Error: two methods provided (info and create). Only one method should be provided.', 'info', 'create')
        self.assert_raises_tool_error('Error parsing command line arguments: Invalid parameter -foo', '-foo')
        self.assert_raises_tool_error('Error loading wallet.dat. Is wallet being used by other process?', '-wallet=wallet.dat', 'info')
        self.assert_raises_tool_error('Error: no wallet file at nonexistent.dat', '-wallet=nonexistent.dat', 'info')

        # Stop the node to close the wallet to call the info command.
        self.log.info('Stop node\n')
        self.stop_node(0)

        # Wallet tool info should work with wallet file permissions set to read-only.
        # This needs to be fixed.
        self.log.info('Set wallet file permissions to read-only')
        os.chmod(self.wallet_path(), 0o400)
        self.log.info('Call wallet tool info')
        self.assert_raises_tool_error('Error loading . Is wallet being used by another process?', 'info')
        self.log.info('Wallet tool info command raises loading error! **FIX ME**')
        self.log.info('Set wallet file permissions back to read/write\n')
        os.chmod(self.wallet_path(), 0o600)

        self.log.info('Call wallet tool info')
        shasum_before = self.wallet_shasum()
        timestamp_before = self.wallet_timestamp('before info')
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
        shasum_after = self.wallet_shasum()
        timestamp_after = self.wallet_timestamp(' after info')
        assert_equal(shasum_after, shasum_before)
        self.log.info('Wallet file shasum unchanged')
        self.log_wallet_timestamp_comparison(timestamp_after, timestamp_before)
        # Wallet tool info should not write to the wallet file.
        # This needs to be fixed.
        assert_greater_than(timestamp_after, timestamp_before)

        # Mutate the wallet to check the info command output changes accordingly.
        self.log.info('Start node')
        self.start_node(0)

        self.log.info('Generate transaction to mutate wallet')
        self.nodes[0].generate(1)

        self.log.info('Stop node\n')
        self.stop_node(0)

        self.log.info('Call wallet tool info after generating a transaction')
        shasum_before = self.wallet_shasum()
        timestamp_before = self.wallet_timestamp('before info')
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
        shasum_after = self.wallet_shasum()
        timestamp_after = self.wallet_timestamp(' after info')
        assert_equal(shasum_after, shasum_before)
        self.log.info('Wallet file shasum unchanged')
        self.log_wallet_timestamp_comparison(timestamp_after, timestamp_before)
        # Wallet tool info should not write to the wallet file.
        # This needs to be fixed.
        assert_greater_than(timestamp_after, timestamp_before)

        self.log.info('Call wallet tool create')
        shasum_before = self.wallet_shasum()
        timestamp_before = self.wallet_timestamp('before create')
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
        shasum_after = self.wallet_shasum()
        timestamp_after = self.wallet_timestamp(' after create')
        assert_equal(shasum_after, shasum_before)
        self.log.info('Wallet file shasum unchanged')
        self.log_wallet_timestamp_comparison(timestamp_after, timestamp_before)
        assert_equal(timestamp_after, timestamp_before)

        self.log.info('Start node with arg -wallet=foo')
        self.start_node(0, ['-wallet=foo'])

        self.log.info('Call getwalletinfo')
        shasum_before = self.wallet_shasum()
        timestamp_before = self.wallet_timestamp('before getwalletinfo')
        out = self.nodes[0].getwalletinfo()
        shasum_after = self.wallet_shasum()
        timestamp_after = self.wallet_timestamp(' after getwalletinfo')
        assert_equal(0, out['txcount'])
        assert_equal(1000, out['keypoolsize'])
        assert_equal(1000, out['keypoolsize_hd_internal'])
        assert_equal(True, 'hdseedid' in out)
        assert_equal(shasum_after, shasum_before)
        self.log.info('Wallet file shasum unchanged')
        self.log_wallet_timestamp_comparison(timestamp_after, timestamp_before)
        assert_equal(timestamp_after, timestamp_before)

        self.log.info('Stop node')
        self.stop_node(0)

if __name__ == '__main__':
    ToolWalletTest().main()
