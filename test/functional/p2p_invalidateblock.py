#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that nodes resync to the main chain after running `invalidateblock`.

1) Generate blocks.
2) Invalidate a block on both nodes using `invalidateblock`.
3) Verify that the nodes resync to the shorter chain.

This test is performed in two different ways to ensure unique blocks:
1) Use generate with one node, then with the other node.
2) Use generatetoaddress with 2 different hardcoded addresses on the same node.

The two tests are order-independent.

"""
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

# Number of blocks to test. See https://github.com/bitcoin/bitcoin/issues/5806.
# The first 16 blocks succeeds because of the call to getdata on processing each
# inv, which occurs until the number of blocks in flight to the peer is maxed
# out. TODO: Fix so that sync doesn't stall at line 81 if BLOCKS is set > 16.
BLOCKS = 16

class P2PInvalidateBlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def assert_block_counts(self, expected):
        for node in self.nodes:
            assert_equal(node.getblockcount(), expected)

    def test_with_generate_to_both_nodes(self):
        self.log.info("Test with generate to node1 then node0")
        block_count = self.nodes[0].getblockcount()

        self.log.info(" Generate blocks on node0")
        block_hashes = self.nodes[0].generate(BLOCKS + 1)
        self.sync_blocks()
        self.assert_block_counts(expected=block_count + BLOCKS + 1)

        self.log.info(" Invalidate a block")
        self.nodes[0].invalidateblock(block_hashes[0])
        self.nodes[1].invalidateblock(block_hashes[0])
        self.assert_block_counts(expected=block_count)

        self.log.info(" Generate an alternate chain on node1")
        # The test framework uses a static per-node address which will generate
        # a deterministic block. Ensure this block is unique by mining a block
        # on node0 to use a different hardcoded address than the one used above
        # for mining on node1.
        self.nodes[1].generate(BLOCKS)
        self.log.info(" Synching")
        self.sync_blocks()

        self.log.info(" Test that nodes re-synched to the new tip")
        self.assert_block_counts(expected=block_count + BLOCKS)

    def test_with_generatetoaddress_node0(self):
        self.log.info("Test with generatetoaddress only to node0")
        deterministic_addrs = ['mjTkW3DjgyZck4KbiRusZsqTgaYTxdSz6z', 'msX6jQXvxiNhx3Q62PKeLPrhrqZQdSimTg']
        block_count = self.nodes[0].getblockcount()

        self.log.info(" Generate blocks to address on node0")
        block_hashes = self.nodes[0].generatetoaddress(BLOCKS + 1, deterministic_addrs[0])
        self.sync_blocks()
        self.assert_block_counts(block_count + BLOCKS + 1)

        self.log.info(" Invalidate a block")
        self.nodes[0].invalidateblock(block_hashes[0])
        self.nodes[1].invalidateblock(block_hashes[0])
        self.assert_block_counts(block_count)

        self.log.info(" Generate an alternate chain to a different address on node0")
        # Generate to a different address to ensure that the coinbase
        # transaction (and therefore also the block hash) is different.
        self.nodes[0].generatetoaddress(BLOCKS, deterministic_addrs[1])
        self.log.info(" Synching")
        self.sync_blocks()

        self.log.info(" Test that nodes re-synched to the new tip")
        self.assert_block_counts(block_count + BLOCKS)

    def run_test(self):
        self.test_with_generate_to_both_nodes()
        self.test_with_generatetoaddress_node0()


if __name__ == '__main__':
    P2PInvalidateBlockTest().main()
