// Copyright (c) 2011-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <consensus/amount.h>
#include <kernel/cs_main.h>
#include <primitives/transaction.h>
#include <rpc/mempool.h>
#include <script/script.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <univalue.h>
#include <util/check.h>

#include <memory>
#include <vector>


static void AddTx(const CTransactionRef& tx, const CAmount& fee, CTxMemPool& pool) EXCLUSIVE_LOCKS_REQUIRED(cs_main, pool.cs)
{
    LockPoints lp;
    TryAddToMempool(pool, CTxMemPoolEntry(tx, fee, /*time=*/0, /*entry_height=*/1, /*entry_sequence=*/0, /*spends_coinbase=*/false, /*sigops_cost=*/4, lp));
}

// Fills the mempool with num_txs small txs, in chains of chain_len txs that each spend the
// previous one.
static void FillMempool(CTxMemPool& pool, int num_txs, int chain_len) EXCLUSIVE_LOCKS_REQUIRED(cs_main, pool.cs)
{
    CTransactionRef prev;
    for (int i = 0; i < num_txs; ++i) {
        CMutableTransaction tx = CMutableTransaction();
        tx.vin.resize(1);
        if (i % chain_len != 0) tx.vin[0].prevout = COutPoint{prev->GetHash(), 0};
        tx.vin[0].scriptSig = CScript() << OP_1;
        tx.vin[0].scriptWitness.stack.push_back({1});
        tx.vout.resize(1);
        tx.vout[0].scriptPubKey = CScript() << OP_1 << OP_EQUAL;
        tx.vout[0].nValue = i;
        prev = MakeTransactionRef(tx);
        // Non-monotonic fees in chains, so that chains are split into several chunks.
        AddTx(prev, /*fee=*/chain_len > 1 ? (i * 7919) % 5000 : i, pool);
    }
    Assert(pool.size() == static_cast<size_t>(num_txs));
}

static void RpcMempoolBench(benchmark::Bench& bench, int num_txs, int chain_len, bool verbose)
{
    const auto testing_setup = MakeNoLogFileContext<const ChainTestingSetup>(ChainType::MAIN);
    CTxMemPool& pool = *Assert(testing_setup->m_node.mempool);
    LOCK2(cs_main, pool.cs);

    FillMempool(pool, num_txs, chain_len);

    bench.run([&] {
        (void)MempoolToJSON(pool, verbose);
    });
}

// Listing all entries in mining order, compared with listing them in no particular order.
static void MempoolEntryOrder(benchmark::Bench& bench, bool in_mining_order)
{
    const auto testing_setup = MakeNoLogFileContext<const ChainTestingSetup>(ChainType::MAIN);
    CTxMemPool& pool = *Assert(testing_setup->m_node.mempool);
    LOCK2(cs_main, pool.cs);

    FillMempool(pool, /*num_txs=*/10000, /*chain_len=*/25);

    bench.run([&] {
        if (in_mining_order) {
            ankerl::nanobench::doNotOptimizeAway(pool.entryAll());
        } else {
            std::vector<CTxMemPoolEntryRef> entries;
            entries.reserve(pool.size());
            for (const CTxMemPoolEntry& e : pool.mapTx) entries.emplace_back(e);
            ankerl::nanobench::doNotOptimizeAway(entries);
        }
    });
}

static void RpcMempool(benchmark::Bench& bench) { RpcMempoolBench(bench, /*num_txs=*/1000, /*chain_len=*/1, /*verbose=*/true); }
static void RpcMempoolTxids(benchmark::Bench& bench) { RpcMempoolBench(bench, /*num_txs=*/10000, /*chain_len=*/25, /*verbose=*/false); }

BENCHMARK(RpcMempool);
static void MempoolEntryAll(benchmark::Bench& bench) { MempoolEntryOrder(bench, /*in_mining_order=*/true); }
static void MempoolEntryUnsorted(benchmark::Bench& bench) { MempoolEntryOrder(bench, /*in_mining_order=*/false); }

BENCHMARK(RpcMempoolTxids);
BENCHMARK(MempoolEntryAll);
BENCHMARK(MempoolEntryUnsorted);
