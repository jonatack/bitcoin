// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <support/allocators/node_allocator.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <list>
#include <map>
#include <string>
#include <unordered_map>

BOOST_FIXTURE_TEST_SUITE(node_allocator_tests, BasicTestingSetup)

#define CHECK_MEMORY_RESOURCE(mr, chunk_size, num_free_chunks, num_blocks) \
    BOOST_CHECK_EQUAL(chunk_size, mr.ChunkSize());                         \
    BOOST_CHECK_EQUAL(num_free_chunks, mr.NumFreeChunks());                \
    BOOST_CHECK_EQUAL(num_blocks, mr.NumBlocks());

BOOST_AUTO_TEST_CASE(too_small)
{
    node_allocator::MemoryResource mr;
    void* ptr = mr.Allocate<char>(1);
    BOOST_CHECK(ptr != nullptr);

    // mr is used
    CHECK_MEMORY_RESOURCE(mr, sizeof(void*), 0, 1);
    mr.Deallocate<char>(ptr, 1);
    CHECK_MEMORY_RESOURCE(mr, sizeof(void*), 1, 1);

    // void* works too, use freelist
    ptr = mr.Allocate<void*>(1);
    BOOST_CHECK(ptr != nullptr);
    CHECK_MEMORY_RESOURCE(mr, sizeof(void*), 0, 1);
    mr.Deallocate<char>(ptr, 1);
    CHECK_MEMORY_RESOURCE(mr, sizeof(void*), 1, 1);
}

BOOST_AUTO_TEST_CASE(std_unordered_map)
{
    using Map = std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, node_allocator::Allocator<std::pair<const uint64_t, uint64_t>>>;

    node_allocator::MemoryResource mr;
    Map m(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr});
    size_t num_free_chunks = 0;
    {
        Map a(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr});

        // Allocator compares equal because the same memory resource is used
        BOOST_CHECK(a.get_allocator() == m.get_allocator());
        for (uint64_t i = 0; i < 1000; ++i) {
            a[i] = i;
        }

        num_free_chunks = mr.NumFreeChunks();

        // create a copy of the map, destroy the map => now a lot more free chunks should be available
        {
            Map b = a;
        }

        BOOST_CHECK(mr.NumFreeChunks() > num_free_chunks);
        num_free_chunks = mr.NumFreeChunks();

        // creating another copy, and then destroying everything should reuse all the chunks
        {
            Map b = a;
        }
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), num_free_chunks);

        // moving the map should not create new nodes
        m = std::move(a);
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), num_free_chunks);
    }
    // a is destroyed, still all chunks should stay roughly the same.
    BOOST_CHECK(mr.NumFreeChunks() <= num_free_chunks + 5);

    m = Map(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr});

    // now we got everything free
    BOOST_CHECK(mr.NumFreeChunks() > num_free_chunks + 50);
}

BOOST_AUTO_TEST_CASE(different_memoryresource_assignment)
{
    using Map = std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, node_allocator::Allocator<std::pair<const uint64_t, uint64_t>>>;

    node_allocator::MemoryResource mr_a;
    node_allocator::MemoryResource mr_b;

    {
        Map map_a(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_a});
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            Map map_b(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_b});
            map_b[123] = 321;
            BOOST_CHECK(map_a.get_allocator() != map_b.get_allocator());
            BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 0);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            map_b = map_a;

            // map_a now uses mr_b, since propagate_on_container_copy_assignment is std::true_type
            BOOST_CHECK(map_a.get_allocator() == map_b.get_allocator());
            BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 1);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            // map_b was now recreated with data from map_a, using mr_a as the memory resource.
        }

        // map_b destroyed, should not have any effect on mr_b
        BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 1);
        BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);
        // but we'll get more free chunks in mr_a
        BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 100);
    }

    // finally map_a is destroyed, getting more free chunks.
    BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 200);
}


BOOST_AUTO_TEST_CASE(different_memoryresource_move)
{
    using Map = std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, node_allocator::Allocator<std::pair<const uint64_t, uint64_t>>>;

    node_allocator::MemoryResource mr_a;
    node_allocator::MemoryResource mr_b;

    {
        Map map_a(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_a});
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            Map map_b(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_b});
            map_b[123] = 321;

            map_b = std::move(map_a);

            // map_a now uses mr_b, since propagate_on_container_copy_assignment is std::true_type
            BOOST_CHECK(map_a.get_allocator() == map_b.get_allocator());
            BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 1);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            // map_b was now recreated with data from map_a, using mr_a as the memory resource.
        }

        // map_b destroyed, should not have any effect on mr_b.
        BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 1);
        BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);
        // but we'll get more free chunks in mr_a
        BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 100);
    }

    // finally map_a is destroyed, but since it was moved, no more free chunks.
    BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 100);
}


BOOST_AUTO_TEST_CASE(different_memoryresource_swap)
{
    using Map = std::unordered_map<uint64_t, uint64_t, std::hash<uint64_t>, std::equal_to<uint64_t>, node_allocator::Allocator<std::pair<const uint64_t, uint64_t>>>;

    node_allocator::MemoryResource mr_a;
    node_allocator::MemoryResource mr_b;

    {
        Map map_a(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_a});
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            Map map_b(0, Map::hasher{}, Map::key_equal{}, Map::allocator_type{&mr_b});
            map_b[123] = 321;

            auto alloc_a = map_a.get_allocator();
            auto alloc_b = map_b.get_allocator();

            std::swap(map_a, map_b);

            // maps have swapped, so their allocator have swapped too. No additional allocations have occored!
            BOOST_CHECK(map_a.get_allocator() != map_b.get_allocator());
            BOOST_CHECK(alloc_a == map_b.get_allocator());
            BOOST_CHECK(alloc_b == map_a.get_allocator());
        }

        // map_b destroyed, so mr_a must have plenty of free chunks now
        BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 100);

        // nothing happened to map_a, so mr_b still has no free chunks
        BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 0);
    }

    // finally map_a is destroyed, so we got an entry back for mr_b.
    BOOST_CHECK_EQUAL(mr_a.NumFreeChunks(), 100);
    BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 1);
}

// some structs that with defined alignment and customizeable size

namespace {

template <size_t S>
struct alignas(1) A1 {
    char data[S];
};

template <size_t S>
struct alignas(2) A2 {
    char data[S];
};

template <size_t S>
struct alignas(4) A4 {
    char data[S];
};

template <size_t S>
struct alignas(8) A8 {
    char data[S];
};

template <size_t S>
struct alignas(16) A16 {
    char data[S];
};

template <size_t S>
struct alignas(32) A32 {
    char data[S];
};

constexpr bool isMultiple(size_t a, size_t b)
{
    return (a / b) * b == a;
}

} // namespace

BOOST_AUTO_TEST_CASE(calc_required_chunk_size)
{
    static_assert(sizeof(A1<1>) == 1U);
    static_assert(std::alignment_of_v<A1<1>> == 1U);

    static_assert(sizeof(A16<1>) == 16U);
    static_assert(std::alignment_of_v<A16<1>> == 16U);
    static_assert(sizeof(A16<16>) == 16U);
    static_assert(std::alignment_of_v<A16<16>> == 16U);
    static_assert(sizeof(A16<24>) == 32U);
    static_assert(std::alignment_of_v<A16<24>> == 16U);

    if (sizeof(void*) == 8U) {
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<1>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<7>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<8>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<9>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<15>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<16>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<17>>(), 24U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<100>>(), 104U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<4>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<7>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<100>>(), 104U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<100>>(), 104U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<1>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<8>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<16>>(), 16U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<1>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<8>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<16>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<17>>(), 32U);
    } else if (sizeof(void*) == 4U) {
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<1>>(), 4U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<7>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<8>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<9>>(), 12U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<15>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<16>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<17>>(), 20U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A1<100>>(), 100U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<4>>(), 4U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<7>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A4<100>>(), 100U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<100>>(), 104U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<1>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<8>>(), 8U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A8<16>>(), 16U);

        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<1>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<8>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<16>>(), 16U);
        BOOST_CHECK_EQUAL(node_allocator::MemoryResource::CalcRequiredChunkSizeBytes<A16<17>>(), 32U);
    }
}

BOOST_AUTO_TEST_SUITE_END()
