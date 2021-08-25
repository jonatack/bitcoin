// Copyright (c) 2019 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <support/allocators/node_allocator.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <cstddef>
#include <list>
#include <map>
#include <string>
#include <type_traits>
#include <unordered_map>

BOOST_FIXTURE_TEST_SUITE(node_allocator_tests, BasicTestingSetup)

#define CHECK_MEMORY_RESOURCE(mr, chunk_size, num_free_chunks, num_blocks) \
    BOOST_CHECK_EQUAL(chunk_size, mr.ChunkSizeBytes());                    \
    BOOST_CHECK_EQUAL(num_free_chunks, mr.NumFreeChunks());                \
    BOOST_CHECK_EQUAL(num_blocks, mr.NumBlocks());

#define CHECK_IN_RANGE(what, lowerInclusive, upperInclusive) \
    BOOST_TEST(what >= lowerInclusive);                      \
    BOOST_TEST(what <= upperInclusive);

BOOST_AUTO_TEST_CASE(too_small)
{
    node_allocator::MemoryResource mr{sizeof(void*)};
    void* ptr{mr.Allocate<char>(1)};
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
    using Factory = node_allocator::UnorderedMapFactory<uint64_t, uint64_t>;

    auto mr = Factory::CreateMemoryResource();
    auto m = Factory::CreateContainer(&mr);
    size_t num_free_chunks = 0;
    {
        auto a = Factory::CreateContainer(&mr);

        // Allocator compares equal because the same memory resource is used
        BOOST_CHECK(a.get_allocator() == m.get_allocator());
        for (uint64_t i = 0; i < 1000; ++i) {
            a[i] = i;
        }

        num_free_chunks = mr.NumFreeChunks();

        // create a copy of the map, destroy the map => now a lot more free chunks should be available
        {
            auto b = a;
        }

        BOOST_CHECK(mr.NumFreeChunks() > num_free_chunks);
        num_free_chunks = mr.NumFreeChunks();

        // creating another copy, and then destroying everything should reuse all the chunks
        {
            auto b = a;
        }
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), num_free_chunks);

        // moving the map should not create new nodes
        m = std::move(a);
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), num_free_chunks);
    }
    // a is destroyed, still all chunks should stay roughly the same.
    BOOST_CHECK(mr.NumFreeChunks() <= num_free_chunks + 5);

    m = Factory::CreateContainer(&mr);

    // now we got everything free
    BOOST_CHECK(mr.NumFreeChunks() > num_free_chunks + 50);
}

BOOST_AUTO_TEST_CASE(different_memoryresource_assignment)
{
    using Factory = node_allocator::UnorderedMapFactory<uint64_t, uint64_t>;

    auto mr_a = Factory::CreateMemoryResource();
    auto mr_b = Factory::CreateMemoryResource();

    {
        auto map_a = Factory::CreateContainer(&mr_a);
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            auto map_b = Factory::CreateContainer(&mr_b);
            map_b[123] = 321;
            BOOST_CHECK(map_a.get_allocator() != map_b.get_allocator());
            BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 0);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            map_b = map_a;

            // map_a now uses mr_b, since propagate_on_container_copy_assignment is std::true_type
            BOOST_CHECK(map_a.get_allocator() == map_b.get_allocator());
            CHECK_IN_RANGE(mr_b.NumFreeChunks(), 1U, 2U);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            // map_b was now recreated with data from map_a, using mr_a as the memory resource.
        }

        // map_b destroyed, should not have any effect on mr_b
        CHECK_IN_RANGE(mr_b.NumFreeChunks(), 1U, 2U);
        BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);
        // but we'll get more free chunks in mr_a
        CHECK_IN_RANGE(mr_a.NumFreeChunks(), 100U, 101U);
    }

    // finally map_a is destroyed, getting more free chunks.
    CHECK_IN_RANGE(mr_a.NumFreeChunks(), 200U, 202U);
}


BOOST_AUTO_TEST_CASE(different_memoryresource_move)
{
    using Factory = node_allocator::UnorderedMapFactory<uint64_t, uint64_t>;

    auto mr_a = Factory::CreateMemoryResource();
    auto mr_b = Factory::CreateMemoryResource();

    {
        auto map_a = Factory::CreateContainer(&mr_a);
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            auto map_b = Factory::CreateContainer(&mr_b);
            map_b[123] = 321;

            map_b = std::move(map_a);

            // map_a now uses mr_b, since propagate_on_container_move_assignment is std::true_type
            BOOST_CHECK(map_a.get_allocator() == map_b.get_allocator());
            CHECK_IN_RANGE(mr_b.NumFreeChunks(), 1U, 2U);
            BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);

            // map_b was now recreated with data from map_a, using mr_a as the memory resource.
        }

        // map_b destroyed, should not have any effect on mr_b.
        CHECK_IN_RANGE(mr_b.NumFreeChunks(), 1U, 2U);
        BOOST_CHECK_EQUAL(mr_b.NumBlocks(), 1);
        // but we'll get more free chunks in mr_a
        CHECK_IN_RANGE(mr_a.NumFreeChunks(), 100U, 101U);
    }

    // finally map_a is destroyed, but since it was moved, no more free chunks.
    CHECK_IN_RANGE(mr_a.NumFreeChunks(), 100U, 102U);
}


BOOST_AUTO_TEST_CASE(different_memoryresource_swap)
{
    using Factory = node_allocator::UnorderedMapFactory<uint64_t, uint64_t>;

    auto mr_a = Factory::CreateMemoryResource();
    auto mr_b = Factory::CreateMemoryResource();

    {
        auto map_a = Factory::CreateContainer(&mr_a);
        for (int i = 0; i < 100; ++i) {
            map_a[i] = i;
        }

        {
            auto map_b = Factory::CreateContainer(&mr_b);
            map_b[123] = 321;

            auto alloc_a = map_a.get_allocator();
            auto alloc_b = map_b.get_allocator();

            std::swap(map_a, map_b);

            // The maps have swapped, so their allocators have swapped, too.
            // No additional allocations have occurred!
            BOOST_CHECK(map_a.get_allocator() != map_b.get_allocator());
            BOOST_CHECK(alloc_a == map_b.get_allocator());
            BOOST_CHECK(alloc_b == map_a.get_allocator());
        }

        // map_b destroyed, so mr_a must have plenty of free chunks now
        CHECK_IN_RANGE(mr_a.NumFreeChunks(), 100U, 101U);

        // nothing happened to map_a, so mr_b still has no free chunks
        BOOST_CHECK_EQUAL(mr_b.NumFreeChunks(), 0);
    }

    // finally map_a is destroyed, so we got an entry back for mr_b.
    CHECK_IN_RANGE(mr_a.NumFreeChunks(), 100U, 101U);
    CHECK_IN_RANGE(mr_b.NumFreeChunks(), 1U, 2U);
}

// some structs with defined alignment and customizeable size

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

namespace {

template <typename T>
struct NotNoexceptHash {
    size_t operator()(const T& x) const /* noexcept */
    {
        return std::hash<T>{}(x);
    }
};

} // namespace

template <typename Key, typename Value, typename Hash = std::hash<Key>>
void TestChunksAreUsed()
{
    using Factory = node_allocator::UnorderedMapFactory<Key, Value, Hash>;
    auto mr = Factory::CreateMemoryResource();
    BOOST_TEST_MESSAGE(strprintf("%u sizeof(void*), %u/%u/%u sizeof Key/Value/Pair, %u mr.ChunkSizeBytes()",
                                 sizeof(void*), sizeof(Key), sizeof(Value), sizeof(std::pair<const Key, Value>), mr.ChunkSizeBytes()));
    {
        auto map = Factory::CreateContainer(&mr);
        for (size_t i = 0; i < 5; ++i) {
            map[i];
        }
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), 0);
        map.clear();
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), 5);

        for (size_t i = 0; i < 5; ++i) {
            map[i];
        }
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), 0);
        map.clear();
        BOOST_CHECK_EQUAL(mr.NumFreeChunks(), 5);
    }

    // makes sure clear frees all chunks. With MSVC there might be an additional chunk used for a control structure.
    CHECK_IN_RANGE(mr.NumFreeChunks(), 5, 6);
}

BOOST_AUTO_TEST_CASE(test_chunks_are_used)
{
#if defined(_LIBCPP_VERSION) // defined in any C++ header from libc++
    BOOST_TEST_MESSAGE("_LIBCPP_VERSION is defined");
#endif
#if defined(__GLIBCXX__) || defined(__GLIBCPP__)
    BOOST_TEST_MESSAGE("__GLIBCXX__ or __GLIBCPP__ is defined");
#endif

    TestChunksAreUsed<uint32_t, uint32_t>();    // 8 byte content
    TestChunksAreUsed<uint64_t, uint32_t>();    // 12 byte content
    TestChunksAreUsed<uint64_t, uint64_t>();    // 16 byte content
    TestChunksAreUsed<uint64_t, std::string>(); // larger
    TestChunksAreUsed<uint64_t, A16<16>>();     // Alignment 16

    TestChunksAreUsed<uint32_t, uint32_t, NotNoexceptHash<uint32_t>>();
    TestChunksAreUsed<uint64_t, uint32_t, NotNoexceptHash<uint64_t>>();
    TestChunksAreUsed<uint64_t, uint64_t, NotNoexceptHash<uint64_t>>();
    TestChunksAreUsed<uint64_t, std::string, NotNoexceptHash<uint64_t>>(); // larger
    TestChunksAreUsed<uint64_t, A16<16>, NotNoexceptHash<uint64_t>>();     // Alignment 16
}

BOOST_AUTO_TEST_SUITE_END()
