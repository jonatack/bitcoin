// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H
#define BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H

#include <memusage.h>

#include <new>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

/**
 * @brief Efficient allocator for node-based containers.
 *
 * The combination of Allocator and MemoryResource can be used as an optimization for node-based containers
 * that experience heavy load.
 *
 * ## Behavior
 *
 * MemoryResource mallocs blocks of memory and uses these to carve out memory for the nodes. Nodes that are
 * destroyed by the Allocator are actually put back into a free list for further use. This behavior has two main advantages:
 *
 * - Memory: no malloc control structure is required for each node memory; the free list is stored in-place. This typically
 *   saves about 16 bytes per node.
 *
 * - Performance: much fewer calls to malloc/free. Accessing / putting back entries are O(1) with low constant overhead.
 *
 * There's no free lunch, so there are also disadvantages:
 *
 * - It is necessary to know the exact size of the container internally used nodes beforehand, but there is no
 *   standard way to get this.
 *
 * - Memory that's been used for nodes is always put back into a free list and never given back to the system. Memory
 *   is only freed when the MemoryResource is destructed.
 *
 * - The free list is a simple last-in-first-out linked list, it doesn't reorder elements based on proximity. So freeing
 *   and malloc'ing again can become a random access pattern which can lead to more cache misses.
 *
 * ## Design & Implementation
 *
 * Allocator is a cheaply copyable, `std::allocator`-compatible type used for the containers. Similar to
 * `std::pmr::polymorphic_allocator`, it holds a pointer to a memory resource.
 *
 * MemoryResource is an immobile object that actually allocates, holds and manages chunks of memory. Currently it is only
 * able provide optimized alloc/free for a single fixed node size which is given in the constructor. Only allocations that match this
 * size will be provided from the preallocated blocks of memory; all other requests simply use ::operator new().
 * To get the correct node size in all cases it is unfortunately necessary to have a look at the various implementations of
 * std::unordered_map. There is currently no standard way of getting the node size programmatically.
 *
 * Node size is determined by memusage::NodeSize and verified to work in various alignment scenarios in
 * `node_allocator_tests/test_chunks_are_used`.
 *
 * ## Further Links
 *
 * @see CppCon 2017: Bob Steagall “How to Write a Custom Allocator” https://www.youtube.com/watch?v=kSWfushlvB8
 * @see C++Now 2018: Arthur O'Dwyer “An Allocator is a Handle to a Heap” https://www.youtube.com/watch?v=0MdSJsCTRkY
 * @see AllocatorAwareContainer: Introduction and pitfalls of propagate_on_container_XXX defaults
 *      https://www.foonathan.net/2015/10/allocatorawarecontainer-propagation-pitfalls/
 */
namespace node_allocator {

/**
 * Actually holds and provides memory to an allocator. MemoryResource is an immobile object. It stores a number of memory blocks
 * (the pool) which are used to quickly give out memory of a fixed chunk size. The class is purposely kept very simple. It only
 * knows about "Chunks" and "Blocks".
 *
 * - Block: MemoryResource allocates one memory block at a time. These blocks are kept around until the memory resource is destroyed.
 *
 * - Chunks: Node-based containers allocate one node at a time. Whenever that happens, the MemoryResource's Allocate() gives out
 *   one chunk of memory. These chunks are carved out from a previously allocated memory block, or from a free list if it contains entries.
 *   Whenever a node is given back with Deallocate(), it is put into that free list.
 */
class MemoryResource
{
    //! Size in bytes to allocate per block, currently hardcoded to 256 KiB.
    static constexpr size_t BlockSizeBytes = 262144;

public:
    /**
     * In-place linked list of the allocation chunks, used for the free list.
     */
    struct ChunkNode {
        void* next;
    };

    /**
     * Construct a new Memory Resource object that uses the specified chunk size to optimize for.
     */
    explicit MemoryResource(size_t chunk_size_bytes) : m_chunk_size_bytes(chunk_size_bytes) {}

    /**
     * Copying/moving a memory resource is not allowed; it is an immobile object.
     */
    MemoryResource(const MemoryResource&) = delete;
    MemoryResource& operator=(const MemoryResource&) = delete;
    MemoryResource(MemoryResource&&) = delete;
    MemoryResource& operator=(MemoryResource&&) = delete;

    /**
     * Deallocates all allocated blocks.
     *
     * There's no Clear() method on purpose, because it would be dangerous. E.g. when calling clear() on
     * an unordered_map, it is not certain that all allocated chunks are given back to the MemoryResource.
     * Microsoft's STL still uses a control structure that might have the same size as the nodes, and therefore
     * needs to be kept around until the map is actually destroyed.
     */
    ~MemoryResource() noexcept
    {
        for (auto* block : m_allocated_blocks) {
            ::operator delete(block);
        }
    }

    /**
     * Allocates memory for n times T. Only when n==1 and T's size matches m_chunk_size_bytes
     * the chunking mechanism is used. Otherwise, the allocation is forwarded to ::operator new().
     *
     * @tparam T Object to allocate memory for.
     * @param n Number of objects to allocate for
     */
    template <typename T>
    [[nodiscard]] T* Allocate(size_t n)
    {
        // assign to a constexpr variable to force constexpr evaluation
        static constexpr auto required_chunk_size = CalcRequiredChunkSizeBytes<T>();

        if (m_chunk_size_bytes != required_chunk_size || n != 1) {
            // pool is not used so forward to operator new.
            return static_cast<T*>(::operator new(n * sizeof(T)));
        }

        // chunk size is correct, so we can actually use the pool's block data

        if (m_free_chunks) {
            // we've already got data in the free list, unlink one element
            auto old_head = m_free_chunks;
            m_free_chunks = static_cast<ChunkNode*>(m_free_chunks)->next;
            return static_cast<T*>(old_head);
        }

        // free list is empty: get one chunk from allocated block memory.
        // It makes sense to not create the fully linked list of an allocated block up-front, for several reasons.
        // On the one hand, the latency is higher when we need to iterate and update pointers for the whole block at once.
        // More importantly, most systems lazily allocate data. So when we allocate a big block of memory the memory for a page
        // is only actually made available to the program when it is first touched. So when we allocate a big block and only use
        // very little memory from it, the total memory usage is lower than what has been malloc'ed.
        if (m_untouched_memory_iterator == m_untouched_memory_end) {
            // slow path, only happens when a new block needs to be allocated
            AllocateNewBlock();
        }

        // peel off one chunk from the untouched memory. The next pointer of in-use elements doesn't matter until it is
        // deallocated, only then it is used to form the free list.
        const auto tmp = m_untouched_memory_iterator;
        m_untouched_memory_iterator = static_cast<char*>(tmp) + m_chunk_size_bytes;
        return static_cast<T*>(tmp);
    }

    /**
     * Puts p back into the free list f it was actually allocated from the memory block.
     * Otherwise, simply call ::operator delete(p).
     */
    template <typename T>
    void Deallocate(void* p, size_t n) noexcept
    {
        // assign to a constexpr variable to force constexpr evaluation
        static constexpr auto required_chunk_size_bytes = CalcRequiredChunkSizeBytes<T>();

        if (m_chunk_size_bytes == required_chunk_size_bytes && n == 1) {
            // put it into the linked list
            const auto node = static_cast<ChunkNode*>(p);
            node->next = m_free_chunks;
            m_free_chunks = node;
        } else {
            // allocation didn't happen with the pool
            ::operator delete(p);
        }
    }

    /**
     * Actual size in bytes that is used for one chunk (node allocation)
     */
    [[nodiscard]] size_t ChunkSizeBytes() const noexcept
    {
        return m_chunk_size_bytes;
    }

    /**
     * Calculates bytes allocated by the memory resource.
     */
    [[nodiscard]] size_t DynamicMemoryUsage() const noexcept
    {
        const size_t alloc_size{(BlockSizeBytes / m_chunk_size_bytes) * m_chunk_size_bytes};
        return memusage::MallocUsage(alloc_size) * m_allocated_blocks.size() + memusage::DynamicUsage(m_allocated_blocks);
    }

    /**
     * Counts number of free entries in the free list. This is an O(n) operation. Mostly for debugging / logging / testing.
     */
    [[nodiscard]] size_t NumFreeChunks() const noexcept
    {
        size_t length = 0;
        auto node = m_free_chunks;
        while (node) {
            node = static_cast<ChunkNode const*>(node)->next;
            ++length;
        }
        return length;
    }

    /**
     * Number of memory blocks that have been allocated
     */
    [[nodiscard]] size_t NumBlocks() const noexcept
    {
        return m_allocated_blocks.size();
    }

    /**
     * Calculates the required chunk size for the given type.
     * The memory block needs to be correctly aligned and large enough to hold both T and ChunkNode.
     */
    template <typename T>
    [[nodiscard]] static constexpr size_t CalcRequiredChunkSizeBytes() noexcept
    {
        const auto alignment_max = std::max(std::alignment_of_v<T>, std::alignment_of_v<ChunkNode>);
        const auto size_max = std::max(sizeof(T), sizeof(ChunkNode));

        // find closest multiple of alignment_max that holds size_max
        return ((size_max + alignment_max - 1U) / alignment_max) * alignment_max;
    }

private:
    /**
     * Allocate one full memory block which is used to carve out chunks.
     * The block size is the multiple of m_chunk_size_bytes that comes closest to BlockSizeBytes.
     */
    void AllocateNewBlock()
    {
        static_assert(sizeof(char) == 1U);

        const auto num_chunks = BlockSizeBytes / m_chunk_size_bytes;
        m_untouched_memory_iterator = ::operator new(num_chunks* m_chunk_size_bytes);
        m_untouched_memory_end = static_cast<char*>(m_untouched_memory_iterator) + num_chunks * m_chunk_size_bytes;
        m_allocated_blocks.push_back(m_untouched_memory_iterator);
    }

    /**
     * The MemoryResource's size for the memory chunks that it can give out.
     */
    size_t const m_chunk_size_bytes;

    //! A contains all allocated blocks of memory, used to free the data in the destructor.
    std::vector<void*> m_allocated_blocks{};

    //! A single linked list of all data available in the MemoryResource. This list is used for allocations of single elements.
    void* m_free_chunks = nullptr;

    //! Points to the beginning of available memory for carving out chunks.
    void* m_untouched_memory_iterator = nullptr;

    //! Points to the end of available memory for carving out chunks.
    void* m_untouched_memory_end = nullptr;
};


/**
 * Allocator that's usable for node-based containers like std::unordered_map or std::list.
 *
 * The allocator is stateful, and can be cheaply copied. Its state is an immobile MemoryResource, which
 * actually does all the allocation/deallocations. So this class is just a simple wrapper that conforms to the
 * required STL interface to be usable for the node-based containers.
 */
template <typename T>
class Allocator
{
    template <typename U>
    friend class Allocator;

    template <typename X, typename Y>
    friend bool operator==(const Allocator<X>& a, const Allocator<Y>& b) noexcept;

public:
    using value_type = T;

    /**
     * The allocator is stateful so we can't use the compile time `is_always_equal` optimization and have to use the runtime operator==.
     */
    using is_always_equal = std::false_type;

    /**
     * Move assignment should be a fast operation. In the case of a = std::move(b), we want
     * a to be able to use b's allocator, otherwise all elements would have to be recreated with a's old allocator.
     */
    using propagate_on_container_move_assignment = std::true_type;

    /**
     * Swapping two containers with unequal allocators who are *not* propagated is undefined
     * behavior. Unfortunately this is the default! Obviously, we don't want that.
     */
    using propagate_on_container_swap = std::true_type; // to avoid the undefined behavior

    /**
     * Move and swap have to propagate the allocator, so for consistency we do the same for copy assignment.
     */
    using propagate_on_container_copy_assignment = std::true_type;

    /**
     * Construct a new Allocator object which will delegate all allocations/deallocations to the memory resource.
     */
    explicit Allocator(MemoryResource* memory_resource) noexcept
        : m_memory_resource(memory_resource)
    {
    }

    /**
     * Conversion constructor for rebinding. All Allocators use the same memory_resource.
     */
    template <typename U>
    Allocator(const Allocator<U>& other) noexcept
        : m_memory_resource(other.m_memory_resource)
    {
    }

    /**
     * Allocates n entries of the given type.
     */
    T* allocate(size_t n)
    {
        // Forward all allocations to the memory_resource
        return m_memory_resource->Allocate<T>(n);
    }


    /**
     * Deallocates n entries of the given type.
     */
    void deallocate(T* p, size_t n)
    {
        m_memory_resource->Deallocate<T>(p, n);
    }

private:
    //! Stateful allocator, where the state is a simple pointer that can be cheaply copied.
    MemoryResource* m_memory_resource;
};


/**
 * Since Allocator is stateful, comparison with another one only returns true if it uses the same memory_resource.
 */
template <typename T, typename U>
bool operator==(const Allocator<T>& a, const Allocator<U>& b) noexcept
{
    // "Equality of an allocator is determined through the ability of allocating memory with one
    // allocator and deallocating it with another." - Jonathan Müller
    // See https://www.foonathan.net/2015/10/allocatorawarecontainer-propagation-pitfalls/
    //
    // For us that is the case when both allocators use the same memory resource.
    return a.m_memory_resource == b.m_memory_resource;
}

template <typename T, typename U>
bool operator!=(const Allocator<T>& a, const Allocator<U>& b) noexcept
{
    return !(a == b);
}

/**
 * Helper to create std::unordered_map which uses the node_allocator.
 *
 * This calculates the size of the container's internally used node correctly for all supported platforms,
 * which is also asserted by the unit tests.
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>, typename Equals = std::equal_to<Key>>
class UnorderedMapFactory
{
public:
    using AllocatorType = Allocator<std::pair<const Key, Value>>;
    using ContainerType = std::unordered_map<Key, Value, Hash, Equals, AllocatorType>;
    static constexpr size_t NodeSizeBytes = memusage::NodeSize<ContainerType>::Value();

    /**
     * Create the MemoryResource with correctly calculated ChunkSize.
     */
    [[nodiscard]] static MemoryResource CreateMemoryResource()
    {
        return MemoryResource{NodeSizeBytes};
    }

    /**
     * Creates the std::unordered_map container, and asserts that the specified memory_resource is correct.
     */
    [[nodiscard]] static ContainerType CreateContainer(MemoryResource* memory_resource)
    {
        assert(memory_resource != nullptr && memory_resource->ChunkSizeBytes() == NodeSizeBytes);
        return ContainerType{0, Hash{}, Equals{}, AllocatorType{memory_resource}};
    }

    /**
     * Constructs MemoryResource in the uninitialized storage pointed to by ptr.
     */
    static void Construct(MemoryResource* ptr)
    {
        assert(ptr != nullptr);
        ::new ((void*)ptr) MemoryResource{NodeSizeBytes};
    }

    /**
     * Constructs the container in the uninitialized storage pointed to by ptr, using the given memory_resource as parameter.
     */
    static void Construct(ContainerType* ptr, MemoryResource* memory_resource)
    {
        assert(ptr != nullptr && memory_resource != nullptr && memory_resource->ChunkSizeBytes() == NodeSizeBytes);
        ::new ((void*)ptr) ContainerType{0, Hash{}, Equals{}, AllocatorType{memory_resource}};
    }
};

} // namespace node_allocator

#endif // BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H
