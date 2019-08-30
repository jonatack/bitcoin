// Copyright (c) 2019-2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H
#define BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @brief Efficient allocator for node-based containers.
 *
 * The combination of Allocator and MemoryResource can be used as an optimization for node based containers
 * that experience heavy load.
 *
 * ## Behavior
 *
 * MemoryResource mallocs blocks of memory and uses these to carve out memory for the nodes. Nodes that are
 * destroyed by the Allocator are actually put back into a freelist for further use.This behavior has two main advantages:
 *
 * - Memory: no control structure is required for each node memory, the freelist is stored inplace. This typically safes
 *   about 8 bytes per node.
 * - Performance: much fewer calls to malloc/free. Accessing / putting back entries are O(1) with low constant overhead.
 *
 * There's no free lunch, so there are also disadvantages:
 *
 * - Memory that's been used for nodes is always put back into a freelist and never given back to the system. Memory
 *   is only freed when the MemoryResource is destructed.
 *
 * - The freelist is a simple first-in-last-out linked list, it doesn't reorder elements. So freeing and malloc'ing again
 *   can have an advantageous access pattern which leads to less cache misses.
 *
 * ## Design & Implementation
 *
 * Allocator is a cheaply copyable, `std::allocator` compatible type used for the containers. Similar to
 * `std::pmr::polymorphic_allocator`, it holds a pointer to a memory resource.
 *
 * MemoryResource is an immobile object that actually allocates, holds and manages chunks of memory. Since there is
 * unfortunately no way to determine the size of nodes that we want to optimize for in advance, MemoryResource uses
 * a simple heuristic: We assume the first call to Allocate with 1 element is for the node, and upon that first call the
 * MemoryResource is configured to use that as it's chunk size. Note that due to this heuristic we cannot implement an
 * `std::pmr::memory_resource` because it does not gie the information about the number of elements.
 *
 * ## Further Links
 *
 * @see CppCon 2017: Bob Steagall “How to Write a Custom Allocator” https://www.youtube.com/watch?v=kSWfushlvB8
 * @see C++Now 2018: Arthur O'Dwyer “An Allocator is a Handle to a Heap” https://www.youtube.com/watch?v=0MdSJsCTRkY
 * @see AllocatorAwareContainer: Introduction and pitfalls of propagate_on_container_XXX defaults
 *     https://www.foonathan.net/2015/10/allocatorawarecontainer-propagation-pitfalls/
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
 *   one chunk of memory. These chunks are carved out from a previously allocated memory block. Whenever a node is given back
 *   with Deallocate(), it is put into a freelist.
 */
class MemoryResource
{
public:
    /**
     * Inplace linked list of the allocation chunks, used for the free list.
     */
    struct ChunkNode {
        void* next;
    };

    /**
     * Construct a new Memory Resource object and uses the specified block size for allocations.
     * Actually the real block size can be a bit smaller, it will be the largest multiple of chunk size
     * that fits into the block.
     */
    explicit MemoryResource(size_t block_size_byte) : m_block_size_bytes(block_size_byte) {}

    MemoryResource() = default;

    /**
     * Copying a memory resource is not allowed, it is an immobile object.
     */
    MemoryResource(const MemoryResource&) = delete;
    MemoryResource& operator=(const MemoryResource&) = delete;

    /**
     * Deallocates all allocated blocks.
     */
    ~MemoryResource() noexcept
    {
        for (auto* block : m_allocated_blocks) {
            ::operator delete(block);
        }
    }

    /**
     * Allocates memory for n times T. Only when n==1 the memory blocks are used to give out memory. The first call with n==1
     * decides the chunk size.
     *
     * @tparam T Object to allocate memory for.
     * @param n Number of objects to allocate for
     */
    template <typename T>
    [[nodiscard]] T* Allocate(size_t n)
    {
        // assign to a constexpr variable to force constexpr evaluation
        static constexpr auto required_chunk_size = CalcRequiredChunkSizeBytes<T>();

        // only use pool when a single element is allocated
        if (m_chunk_size_bytes == 0 && n == 1) {
            // first call to Allocate with n==1 determines chunk size
            m_chunk_size_bytes = required_chunk_size;
        }

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

        // freelist is empty: get one chunk from allocated block memory.
        // It makes sense to not create the fully linked list of an allocated block up front, for several reasons
        // On the one hand, zhe latency is higher when we need to iterate and update pointers for the whole block at once.
        // More importantly, most systems lazily allocate data. So when we allocate a big block of memory the memory for a page
        // is only actually made available to the program when it is first touched. So when we allocate a big block and only use
        // very little memory from it, the total memory usage is lower than what has been malloced'.
        if (m_untouched_memory_iterator == m_untouched_memory_end) {
            // slow path, only happens when a new block needs to be allocated
            AllocateNewBlock();
        }

        // peel off one chunk from the untouched memory
        auto tmp = m_untouched_memory_iterator;
        m_untouched_memory_iterator = static_cast<char*>(tmp) + m_chunk_size_bytes;
        return static_cast<T*>(tmp);
    }

    /**
     * Puts p back into the freelist f it was actually allocated form the memory block.
     */
    template <typename T>
    void Deallocate(void* p, std::size_t n) noexcept
    {
        // assign to a constexpr variable to force constexpr evaluation
        static constexpr auto required_chunk_size_bytes = CalcRequiredChunkSizeBytes<T>();

        if (m_chunk_size_bytes == required_chunk_size_bytes && n == 1) {
            // put it into the linked list
            auto node = static_cast<ChunkNode*>(p);
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
    [[nodiscard]] size_t ChunkSize() const noexcept
    {
        return m_chunk_size_bytes;
    }

    /**
     * Counts number of free entries in the freelist. This is an O(n) operation. Mostly for debugging / logging / testing.
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
        auto alignment_max = std::max(std::alignment_of_v<T>, std::alignment_of_v<ChunkNode>);
        auto size_max = std::max(sizeof(T), sizeof(ChunkNode));

        // find closest multiple of alignment_max that holds size_max
        return ((size_max + alignment_max - 1U) / alignment_max) * alignment_max;
    }

private:
    /**
     * Allocate one full memory block which is used to carve out chunks.
     * The block size is the multiple of m_chunk_size_bytes that comes closest to m_block_size_bytes.
     */
    void AllocateNewBlock()
    {
        static_assert(sizeof(char) == 1U);

        auto const num_chunks = m_block_size_bytes / m_chunk_size_bytes;
        m_untouched_memory_iterator = ::operator new(num_chunks* m_chunk_size_bytes);
        m_untouched_memory_end = static_cast<char*>(m_untouched_memory_iterator) + num_chunks * m_chunk_size_bytes;
        m_allocated_blocks.push_back(m_untouched_memory_iterator);
    }

    //! A single linked list of all data available in the pool. This list is used for allocations of single elements.
    void* m_free_chunks = nullptr;

    //! A contains all allocated blocks of memory, used to free the data in the destructor.
    std::vector<void*> m_allocated_blocks{};

    //! The pool's size for the memory blocks. First call to Allocate() determines the used size.
    size_t m_chunk_size_bytes{0};

    //! Size in bytes to allocate per block. Defaults to 256 KiB
    size_t const m_block_size_bytes{262144};

    //! Points to the begin of available memory for carving out chunks.
    void* m_untouched_memory_iterator = nullptr;

    //! Points to the end of available memory for carving out chunks.
    void* m_untouched_memory_end = nullptr;
};


/**
 * Allocator that's usable for node-based containers like std::unorderd_map or std::list.
 *
 * The allocator is stateful, and can be cheaply copied. Its state is an immobile MemoryResource, which
 * actually does all the allocation/deallocations. So this class is just a simple wrapper that conforms to the
 * required STL interface. to be usable for the node based containers.
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
     * Move and swap have to propagate the allocator, so for consistency we do he same for copy assignment.
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
    void deallocate(T* p, std::size_t n)
    {
        m_memory_resource->Deallocate<T>(p, n);
    }

private:
    //! Stateful allocator, where the state is a simple pointer that an be cheaply copied.
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

} // namespace node_allocator

#endif // BITCOIN_SUPPORT_ALLOCATORS_NODE_ALLOCATOR_H