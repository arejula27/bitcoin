// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <bench/index_sync_util.h>
#include <index/txospenderindex.h>
#include <interfaces/chain.h>
#include <test/util/setup_common.h>

#include <memory>

// End-to-end sync benchmark for TxoSpenderIndex, exercising the real,
// unmodified index code (BaseIndex::Sync -> TxoSpenderIndex::CustomAppend ->
// BuildSpenderPositions) against a chain with realistic multi-input blocks.
// Unlike BlockFilterIndexSync (src/bench/index_blockfilter.cpp), which only
// uses coinbase-only blocks, ExtendChainWithSpends (src/bench/index_sync_util.h)
// builds a chain of blocks with chained, validly-signed transactions so the
// benchmark actually has meaningful input volume to index.
static void TxoSpenderIndexSync(benchmark::Bench& bench)
{
    const auto test_setup = MakeNoLogFileContext<TestChain100Setup>();
    ExtendChainWithSpends(*test_setup, /*num_blocks=*/20, /*num_txs_per_block=*/500);

    BenchIndexSync(bench, *test_setup, [&] {
        return std::make_unique<TxoSpenderIndex>(interfaces::MakeChain(test_setup->m_node),
                                                  /*n_cache_size=*/1 << 20, /*f_memory=*/true, /*f_wipe=*/true);
    });
}

BENCHMARK(TxoSpenderIndexSync);
