// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>
#include <coins.h>
#include <support/allocators/node_allocator.h>

#include <cstring>
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

    COutPoint p(tx.GetHash(), 0);

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
    using Map = std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher>;
    Map map;
    BenchFillClearMap(bench, map);
}

static void NodeAllocator_StdUnorderedMapWithNodeAllocator(benchmark::Bench& bench)
{
    using Map = std::unordered_map<COutPoint, CCoinsCacheEntry, SaltedOutpointHasher, std::equal_to<COutPoint>, node_allocator::Allocator<std::pair<const COutPoint, CCoinsCacheEntry>>>;
    node_allocator::MemoryResource pool;
    Map map(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&pool});
    BenchFillClearMap(bench, map);
}

BENCHMARK(NodeAllocator_StdUnorderedMap);
BENCHMARK(NodeAllocator_StdUnorderedMapWithNodeAllocator);
