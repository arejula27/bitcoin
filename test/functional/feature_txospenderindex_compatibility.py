#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test txospenderindex compatibility with the legacy (disk position) format.

"""

import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet


class TxoSpenderIndexCompatibilityTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [["-txospenderindex"], ["-txospenderindex"]]

    def skip_test_if_missing_module(self):
        self.skip_if_no_previous_releases()

    def setup_nodes(self):
        self.add_nodes(
            self.num_nodes,
            extra_args=self.extra_args,
            versions=[
                None,
                310000,
            ],
        )
        self.start_nodes()

    def spent_prevout(self, tx):
        prevout = tx["tx"].vin[0].prevout
        return {"txid": f"{prevout.hash:064x}", "vout": prevout.n}

    def assert_spender(self, node, tx, blockhash):
        result = node.gettxspendingprevout([self.spent_prevout(tx)])
        assert_equal(result, [self.spent_prevout(tx) | {"spendingtxid": tx["txid"], "blockhash": blockhash}])

    def assert_unspent(self, node, tx):
        assert_equal(node.gettxspendingprevout([self.spent_prevout(tx)]), [self.spent_prevout(tx)])

    def wait_synced(self, node):
        self.wait_until(lambda: node.getindexinfo()["txospenderindex"]["synced"])

    def run_test(self):
        node = self.nodes[0]
        legacy_node = self.nodes[1]
        self.wallet = MiniWallet(node)

        tx1 = self.wallet.send_self_transfer(from_node=node)
        blockhash1 = self.generate(node, 1)[0]
        for n in self.nodes:
            self.wait_synced(n)

        self.log.info("Ensure that queries to the txospenderindex are consistent between the different index versions")
        self.assert_spender(node, tx1, blockhash1)
        self.assert_spender(legacy_node, tx1, blockhash1)

        self.log.info("Exercise the new index running on a datadir with the old version")
        self.stop_nodes()
        self.cleanup_folder(node.chain_path)
        shutil.copytree(legacy_node.chain_path, node.chain_path)
        self.log.info("Ensure that prune mode rejects a legacy txospenderindex")
        node.assert_start_raises_init_error(
            extra_args=["-prune=1", "-txospenderindex"],
            expected_msg="Error: Prune mode is incompatible with a txospenderindex that still contains legacy entries. Stop the node, delete the txospenderindex directory, and restart to rebuild the index.",
        )
        with node.assert_debug_log(expected_msgs=["txospenderindex contains entries in the legacy format"]):
            self.start_node(0)
        self.wait_synced(node)
        self.assert_spender(node, tx1, blockhash1)

        self.log.info("Test that looking up a newly added spend in a mixed-format db is possible")
        tx2 = self.wallet.send_self_transfer(from_node=node)
        blockhash2 = self.generate(node, 1, sync_fun=self.no_op)[0]
        self.wait_synced(node)
        self.assert_spender(node, tx1, blockhash1)
        self.assert_spender(node, tx2, blockhash2)

        self.log.info("Test that spends of blocks disconnected in a mixed-format db are not returned")
        node.invalidateblock(blockhash1)
        # Mine empty blocks, so that both spends are only left in the mempool.
        for _ in range(3):
            self.generateblock(node, output=self.wallet.get_address(), transactions=[], sync_fun=self.no_op)
        self.restart_node(0, extra_args=["-txospenderindex", "-persistmempool=0"])
        self.wait_synced(node)
        self.assert_unspent(node, tx1)
        self.assert_unspent(node, tx2)

        self.log.info("Test that the old version does not return them either after a downgrade, as it rewinds the blocks disconnected since its locator")
        self.stop_node(0)
        self.cleanup_folder(legacy_node.chain_path)
        shutil.copytree(node.chain_path, legacy_node.chain_path)
        self.start_node(1, extra_args=["-txospenderindex", "-persistmempool=0"])
        self.wait_synced(legacy_node)
        self.assert_unspent(legacy_node, tx1)
        self.assert_unspent(legacy_node, tx2)

        self.log.info("Check rebuild advice after pruning with the legacy txospenderindex disabled")
        self.start_node(0, extra_args=["-fastprune", "-prune=1", "-txospenderindex=0"])
        self.generate(node, 600, sync_fun=self.no_op)
        node.pruneblockchain(node.getblockcount())
        self.stop_node(0)
        node.assert_start_raises_init_error(
            extra_args=["-prune=1", "-txospenderindex"],
            expected_msg="Error: Prune mode is incompatible with a txospenderindex that still contains legacy entries. Restart with -reindex to rebuild the index and redownload the blockchain.",
        )


if __name__ == '__main__':
    TxoSpenderIndexCompatibilityTest(__file__).main()
