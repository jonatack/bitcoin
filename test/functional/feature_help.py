#!/usr/bin/env python3
# Copyright (c) 2018-2019 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test starting bitcoind with -help, -version, and invalid args/tokens."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

HELP_ARGS      = ['-h', '--h', '-help', '--help', '-?', '--?']
VERSION_ARGS   = ['-version', '--version']
INVALID_ARGS   = ['---version', '-v', '--v', '---help']
INVALID_TOKENS = ['help', 'h', '?', 'v', 'version']

HELP_PHRASES = ['Usage:  bitcoind [options]', 'Options:', '-?', 'Print this help message']
INVALID_PARAMETER_ERROR = 'Error parsing command line arguments: Invalid parameter '
SPACING = 10

def test_and_return_node_output(node, expected_return_code):
    return_code = node.process.wait(timeout=5)
    assert_equal(return_code, expected_return_code)

    node.stdout.seek(0)
    node.stderr.seek(0)
    out = node.stdout.read().decode()
    err = node.stderr.read().decode()
    node.stdout.close()
    node.stderr.close()

    # Clean up TestNode state.
    node.running = False
    node.process = None
    node.rpc_connected = False
    node.rpc = None

    return out, err

class HelpTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self):
        self.add_nodes(self.num_nodes)
        # Don't start the node.

    def run_test(self):
        node = self.nodes[0]

        for arg in HELP_ARGS:
            self.log.info('Testing that bitcoind {} exits and outputs help text to stdout'.format(arg.ljust(SPACING)))
            node.start(extra_args=[arg])
            output, _ = test_and_return_node_output(node, expected_return_code=0)
            for phrase in HELP_PHRASES:
                assert phrase in output

        for arg in VERSION_ARGS:
            self.log.info('Testing that bitcoind {} exits and outputs version text to stdout'.format(arg.ljust(SPACING)))
            node.start(extra_args=[arg])
            output, _ = test_and_return_node_output(node, expected_return_code=0)
            assert 'Bitcoin Core Daemon version v' in output

        for arg in INVALID_ARGS:
            self.log.info('Testing that bitcoind {} raises invalid parameter error to stdout'.format(arg.ljust(SPACING)))
            node.start(extra_args=[arg])
            _, output = test_and_return_node_output(node, expected_return_code=1)
            assert INVALID_PARAMETER_ERROR in output

        for arg in INVALID_TOKENS:
            self.log.info('Testing that bitcoind {} raises unexpected token error to stdout'.format(arg.ljust(SPACING)))
            node.assert_start_raises_init_error([arg], "Error: Command line contains unexpected token '{}', see bitcoind -h for a list of options.".format(arg))

if __name__ == '__main__':
    HelpTest().main()
