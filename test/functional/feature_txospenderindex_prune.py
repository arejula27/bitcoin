#!/usr/bin/env python3
# Copyright (c) The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test txospenderindex lookups in prune mode and fetching missing blocks."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from test_framework.wallet import MiniWallet, getnewdestination


class TxoSpenderIndexPruneTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        self.extra_args = [
            ["-fastprune", "-prune=1", "-txospenderindex"],
            [],
        ]

    def run_test(self):
        pruned, full = self.nodes

        self.log.info("A txospenderindex can sync from genesis with pruning enabled")
        wallet = MiniWallet(full)
        self.generate(wallet, 101)
        tx = wallet.send_to(from_node=pruned, scriptPubKey=getnewdestination()[1], amount=100_000)
        blockhash = self.generate(pruned, 1)[0]
        prevout = tx["tx"].vin[0].prevout
        spent = {"txid": f"{prevout.hash:064x}", "vout": prevout.n}
        change = wallet.get_utxo(txid=tx["txid"])
        unspent = {"txid": tx["txid"], "vout": change["vout"]}
        assert_equal(pruned.gettxspendingprevout([spent, unspent]), [spent | {"spendingtxid": tx["txid"], "blockhash": blockhash}, unspent])

        self.log.info("Mine the spend into a competing block, which becomes stale")
        for node in self.nodes:
            node.invalidateblock(blockhash)
        stale_hash = self.generateblock(full, output=wallet.get_address(), transactions=[tx["txid"]])["hash"]
        for node in self.nodes:
            node.reconsiderblock(blockhash)
            assert_equal(node.getbestblockhash(), blockhash)

        self.log.info("Mine enough blocks to prune the spending transaction's block")
        self.generate(full, 600)
        pruned.pruneblockchain(pruned.getblockcount())

        self.log.info("gettxspendingprevout returns the pruned block hash per output and still answers the others")
        assert_equal(pruned.gettxspendingprevout([spent, unspent]), [spent | {"pruned_block_hashes": [blockhash]}, unspent])

        self.log.info("A spend in the mempool is still returned")
        mempool_tx = wallet.send_self_transfer(from_node=pruned, utxo_to_spend=change)
        assert_equal(pruned.gettxspendingprevout([unspent]), [unspent | {"spendingtxid": mempool_tx["txid"]}])

        self.log.info("A readable stale copy of the spend does not hide the pruned active chain block")
        pruned.submitblock(full.getblock(stale_hash, 0))
        assert_equal(pruned.getblock(stale_hash)["confirmations"], -1)
        assert_equal(pruned.gettxspendingprevout([spent]), [spent | {"pruned_block_hashes": [blockhash]}])

        self.log.info("Fetch the pruned block and retry the lookup")
        pruned.getblockfrompeer(blockhash, pruned.getpeerinfo()[0]["id"])
        self.wait_until(lambda: "spendingtxid" in pruned.gettxspendingprevout([spent])[0])
        assert_equal(pruned.gettxspendingprevout([spent], return_spending_tx=True), [spent | {"spendingtxid": tx["txid"], "spendingtx": tx["hex"], "blockhash": blockhash}])


if __name__ == '__main__':
    TxoSpenderIndexPruneTest(__file__).main()
