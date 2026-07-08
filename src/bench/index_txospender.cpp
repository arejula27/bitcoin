// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <bench/bench.h>
#include <consensus/amount.h>
#include <index/base.h>
#include <index/txospenderindex.h>
#include <interfaces/chain.h>
#include <key.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <uint256.h>
#include <validation.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace {

// Extends the active chain with num_blocks blocks of num_txs_per_block
// transactions each. Every transaction spends the two outputs created by the
// previous one and creates two new ones (~2 inputs/tx, matching the average
// ratio of inputs to transactions observed on mainnet), so that
// TxoSpenderIndex has a realistic, non-trivial number of inputs to index per
// block instead of just coinbases.
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

} // namespace

// End-to-end sync benchmark for TxoSpenderIndex, exercising the real,
// unmodified index code (BaseIndex::Sync -> TxoSpenderIndex::CustomAppend ->
// BuildSpenderPositions) against a chain with realistic multi-input blocks.
// Unlike BlockFilterIndexSync (src/bench/index_blockfilter.cpp), which only
// uses coinbase-only blocks, this benchmark builds a chain of blocks with
// chained, validly-signed transactions so the benchmark actually has
// meaningful input volume to index.
static void TxoSpenderIndexSync(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<TestChain100Setup>();
    ExtendChainWithSpends(*test_setup, /*num_blocks=*/20, /*num_txs_per_block=*/500);

    bench.minEpochIterations(5).run([&] {
        TxoSpenderIndex txospenderindex(interfaces::MakeChain(test_setup->m_node),
                                        /*n_cache_size=*/1 << 20, /*f_memory=*/true, /*f_wipe=*/true);
        assert(txospenderindex.Init());
        assert(!txospenderindex.BlockUntilSyncedToCurrentChain());
        txospenderindex.Sync();

        IndexSummary summary = txospenderindex.GetSummary();
        assert(summary.synced);
        assert(summary.best_block_hash == WITH_LOCK(::cs_main, return test_setup->m_node.chainman->ActiveTip()->GetBlockHash()));

        // Shutdown sequence (c.f. Shutdown() in init.cpp)
        txospenderindex.Stop();
    });
}

BENCHMARK(TxoSpenderIndexSync);
