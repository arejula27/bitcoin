// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/index_sync_util.h>

#include <addresstype.h>
#include <consensus/amount.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cstddef>
#include <utility>
#include <vector>

void ExtendChainWithSpends(TestChain100Setup& test_setup, size_t num_blocks, size_t num_txs_per_block)
{
    Chainstate& chainstate{test_setup.m_node.chainman->ActiveChainstate()};

    CKey key_a{GenerateRandomKey()};
    CKey key_b{GenerateRandomKey()};
    const std::vector<CKey> keys{test_setup.coinbaseKey, key_a, key_b};
    const std::vector<CTxOut> outputs{
        CTxOut{COIN, GetScriptForDestination(WitnessV0KeyHash{key_a.GetPubKey()})},
        CTxOut{COIN, GetScriptForDestination(WitnessV0KeyHash{key_b.GetPubKey()})},
    };
    const CScript coinbase_spk{GetScriptForDestination(WitnessV0KeyHash{test_setup.coinbaseKey.GetPubKey()})};

    // Coinbase from block 1 matures exactly when spent at height 101 (chain
    // tip is at height 100 right after TestChain100Setup).
    auto& coinbase_to_spend{test_setup.m_coinbase_txns[0]};
    const auto [first_tx, first_fee]{test_setup.CreateValidTransaction(
        {coinbase_to_spend}, {COutPoint(coinbase_to_spend->GetHash(), 0)},
        chainstate.m_chain.Height() + 1, keys, outputs, {}, {})};
    test_setup.CreateAndProcessBlock({first_tx}, coinbase_spk);

    CTransactionRef tx_to_spend{MakeTransactionRef(first_tx)};
    for (size_t b{0}; b < num_blocks; ++b) {
        std::vector<CMutableTransaction> txs;
        txs.reserve(num_txs_per_block);
        for (size_t i{0}; i < num_txs_per_block; ++i) {
            const std::vector<COutPoint> inputs{
                COutPoint(tx_to_spend->GetHash(), 0),
                COutPoint(tx_to_spend->GetHash(), 1),
            };
            const auto [tx, fee]{test_setup.CreateValidTransaction(
                {tx_to_spend}, inputs, chainstate.m_chain.Height() + 1, keys, outputs, {}, {})};
            txs.emplace_back(tx);
            tx_to_spend = MakeTransactionRef(tx);
        }
        test_setup.CreateAndProcessBlock(txs, coinbase_spk);
    }
}
