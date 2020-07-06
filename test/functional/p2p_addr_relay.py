#!/usr/bin/env python3
# Copyright (c) 2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test addr relay."""

import time

from test_framework.messages import (
    CAddress,
    msg_addr,
    NODE_NETWORK,
    NODE_WITNESS,
)
from test_framework.mininode import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_greater_than_or_equal,
)

ONE_MINUTE  = 60
TEN_MINUTES = 10 * ONE_MINUTE
HALF_HOUR   =  3 * TEN_MINUTES
ONE_HOUR    =  2 * HALF_HOUR
TWO_HOURS   =  2 * ONE_HOUR
ONE_DAY     = 12 * TWO_HOURS

ADDR_DESTINATIONS_THRESHOLD = 4


def gen_addrs(n, time):
    addrs = []
    for i in range(n):
        addr = CAddress()
        addr.time = time + i
        addr.nServices = NODE_NETWORK | NODE_WITNESS
        addr.ip = "123.123.123.{}".format(i % 256)
        addr.port = 8333 + i
        addrs.append(addr)
    return addrs


class AddrReceiver(P2PInterface):
    received_addr = False

    def on_addr(self, message):
        for addr in message.addrs:
            assert_equal(addr.nServices, 9)
            assert addr.ip.startswith('123.123.123.')
            assert (8333 <= addr.port < 8343)
        self.received_addr = True


class AddrTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = False
        self.num_nodes = 1

    def test_sending_oversized_addr(self):
        self.log.info('Test sending too-large addr message')
        self.msg.addrs = gen_addrs(1010, int(time.time()))
        with self.nodes[0].assert_debug_log(['addr message size = 1010']):
            self.conn.send_and_ping(self.msg)

    def test_addr_msg_relay(self):
        self.log.info('Test addr message content is relayed and added to addrman')
        self.conn_addr_receiver = self.nodes[0].add_p2p_connection(AddrReceiver())
        self.mocked_time = int(time.time())
        self.nodes[0].setmocktime(self.mocked_time)
        self.msg.addrs = gen_addrs(10, self.mocked_time)
        msgs = ['Added 10 addresses from 127.0.0.1: 0 tried', 'received: addr (301 bytes) peer=0', 'sending addr (301 bytes) peer=1']
        with self.nodes[0].assert_debug_log(msgs):
            self.conn.send_and_ping(self.msg)
            self.mocked_time += HALF_HOUR
            self.nodes[0].setmocktime(self.mocked_time)
            self.conn_addr_receiver.sync_with_ping()

    def get_nodes_that_received_addr(self, time_interval_1, time_interval_2):
        for _ in range(10):
            self.mocked_time += time_interval_1
            self.msg.addrs[0].time = self.mocked_time + TEN_MINUTES
            self.nodes[0].setmocktime(self.mocked_time)
            with self.nodes[0].assert_debug_log(['received: addr (31 bytes) peer=0']):
                self.conn.send_and_ping(self.msg)
                self.mocked_time += time_interval_2
                self.nodes[0].setmocktime(self.mocked_time)
                self.conn_addr_receiver.sync_with_ping()
        return [node for node in self.addr_receivers if node.received_addr]

    def test_addr_destination_rotates_once_in_24_hours(self):
        self.log.info('Test within 24 hours an addr relay destination is rotated at most once')
        self.msg.addrs = gen_addrs(1, self.mocked_time)
        self.conn_addr_receiver.received_addr = False
        self.addr_receivers = [self.conn_addr_receiver]
        for _ in range(20):
            self.conn_addr_receiver = self.nodes[0].add_p2p_connection(AddrReceiver())
            self.addr_receivers.append(self.conn_addr_receiver)
        nodes_received_addr = self.get_nodes_that_received_addr(0, TWO_HOURS)  # 10 intervals of 2 hours
        # Per RelayAddress, we would announce these addrs to 2 destinations per day.
        # Since it's at most one rotation, at most 4 nodes can receive ADDR.
        assert_greater_than_or_equal(ADDR_DESTINATIONS_THRESHOLD, len(nodes_received_addr))

    def test_addr_destination_rotates_more_than_once_over_several_days(self):
        self.log.info('Test after several days an addr relay destination is rotated more than once')
        for node in self.addr_receivers:
            node.received_addr = False
        # 10 intervals of 1 day (+ 1 hour, which should be enough to cover 30-min Poisson in most cases)
        nodes_received_addr = self.get_nodes_that_received_addr(ONE_DAY, ONE_HOUR)
        # Now that there should have been more than one rotation, more than
        # ADDR_DESTINATIONS_THRESHOLD nodes should have received ADDR.
        assert_greater_than(len(nodes_received_addr), ADDR_DESTINATIONS_THRESHOLD)

    def run_test(self):
        self.conn = self.nodes[0].add_p2p_connection(P2PInterface())
        self.msg = msg_addr()

        self.test_sending_oversized_addr()
        self.test_addr_msg_relay()
        self.test_addr_destination_rotates_once_in_24_hours()
        self.test_addr_destination_rotates_more_than_once_over_several_days()


if __name__ == '__main__':
    AddrTest().main()
