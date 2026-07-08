// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_BENCH_INDEX_SYNC_UTIL_H
#define BITCOIN_BENCH_INDEX_SYNC_UTIL_H

#include <bench/bench.h>
#include <index/base.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <validation.h>

#include <cassert>
#include <cstddef>
#include <memory>

/**
 * Extends the active chain of `test_setup` with `num_blocks` blocks of
 * `num_txs_per_block` chained, validly-signed transactions each. Every
 * transaction spends the two outputs created by the previous one and
 * creates two new ones (~2 inputs/tx, matching the average ratio of inputs
 * to transactions observed on mainnet).
 *
 * Shared by any index benchmark that needs realistic per-block input/output
 * volume: BlockFilterIndexSync (src/bench/index_blockfilter.cpp) only uses
 * coinbase-only blocks, which is fine for that benchmark's purpose but isn't
 * representative for anything that scales with the number of inputs or
 * outputs per block.
 */
void ExtendChainWithSpends(TestChain100Setup& test_setup, size_t num_blocks, size_t num_txs_per_block);

/**
 * Times a full Init -> BlockUntilSyncedToCurrentChain -> Sync cycle of an
 * arbitrary index against the chain already built in `test_setup`.
 * `make_index` is called once per benchmark iteration and must return a
 * fresh, not-yet-initialized `std::unique_ptr<BaseIndex>` (indices can't be
 * re-synced after Stop()).
 */
template <typename MakeIndex>
void BenchIndexSync(benchmark::Bench& bench, TestChain100Setup& test_setup, MakeIndex make_index)
{
    bench.minEpochIterations(5).run([&] {
        std::unique_ptr<BaseIndex> index{make_index()};
        assert(index->Init());
        assert(!index->BlockUntilSyncedToCurrentChain());
        index->Sync();

        IndexSummary summary{index->GetSummary()};
        assert(summary.synced);
        assert(summary.best_block_hash == WITH_LOCK(::cs_main, return test_setup.m_node.chainman->ActiveTip()->GetBlockHash()));

        // Shutdown sequence (c.f. Shutdown() in init.cpp)
        index->Stop();
    });
}

#endif // BITCOIN_BENCH_INDEX_SYNC_UTIL_H
