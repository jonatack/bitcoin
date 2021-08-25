// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <coins.h>
#include <memusage.h>
#include <support/allocators/node_allocator.h>

#include <cstring>
#include <iostream>
#include <unordered_map>

template <typename Map>
void BenchFillClearMap(benchmark::Bench& bench, Map& map)
{
    CMutableTransaction tx = CMutableTransaction();
    tx.vin.resize(1);
    tx.vin[0].scriptSig = CScript() << OP_2;
    tx.vin[0].scriptWitness.stack.push_back({2});
    tx.vout.resize(1);
    tx.vout[0].scriptPubKey = CScript() << OP_2 << OP_EQUAL;
    tx.vout[0].nValue = 10 * COIN;

    COutPoint p{tx.GetHash(), 0};

    bench.epochIterations(5000 * 10).run([&] {
        // modify hash a bit so we get a new entry in the map
        ++p.n;

        map[p];
        if (map.size() >= 5000) {
            map.clear();
        }
    });
}

static void NodeAllocator_StdUnorderedMap(benchmark::Bench& bench)
{
    auto map = std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher>();
    BenchFillClearMap(bench, map);
}

static void NodeAllocator_StdUnorderedMapWithNodeAllocator(benchmark::Bench& bench)
{
    auto memory_resource = CCoinsMapFactory::CreateMemoryResource();
    auto map = CCoinsMapFactory::CreateContainer(&memory_resource);
    BenchFillClearMap(bench, map);
}

BENCHMARK(NodeAllocator_StdUnorderedMap);
BENCHMARK(NodeAllocator_StdUnorderedMapWithNodeAllocator);
