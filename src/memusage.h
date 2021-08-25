// Copyright (c) 2015-2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_MEMUSAGE_H
#define BITCOIN_MEMUSAGE_H

#include <indirectmap.h>
#include <prevector.h>

#include <stdlib.h>

#include <cassert>
#include <map>
#include <memory>
#include <set>
#include <vector>
#include <unordered_map>
#include <unordered_set>

namespace node_allocator {

template <typename T>
class Allocator;

}

namespace memusage
{

/** Compute the total memory used by allocating alloc bytes. */
static size_t MallocUsage(size_t alloc);

/** Dynamic memory usage for built-in types is zero. */
static inline size_t DynamicUsage(const int8_t& v) { return 0; }
static inline size_t DynamicUsage(const uint8_t& v) { return 0; }
static inline size_t DynamicUsage(const int16_t& v) { return 0; }
static inline size_t DynamicUsage(const uint16_t& v) { return 0; }
static inline size_t DynamicUsage(const int32_t& v) { return 0; }
static inline size_t DynamicUsage(const uint32_t& v) { return 0; }
static inline size_t DynamicUsage(const int64_t& v) { return 0; }
static inline size_t DynamicUsage(const uint64_t& v) { return 0; }
static inline size_t DynamicUsage(const float& v) { return 0; }
static inline size_t DynamicUsage(const double& v) { return 0; }
template<typename X> static inline size_t DynamicUsage(X * const &v) { return 0; }
template<typename X> static inline size_t DynamicUsage(const X * const &v) { return 0; }

/** Compute the memory used for dynamically allocated but owned data structures.
 *  For generic data types, this is *not* recursive. DynamicUsage(vector<vector<int> >)
 *  will compute the memory used for the vector<int>'s, but not for the ints inside.
 *  This is for efficiency reasons, as these functions are intended to be fast. If
 *  application data structures require more accurate inner accounting, they should
 *  iterate themselves, or use more efficient caching + updating on modification.
 */

static inline size_t MallocUsage(size_t alloc)
{
    // Measured on libc6 2.19 on Linux.
    if (alloc == 0) {
        return 0;
    } else if (sizeof(void*) == 8) {
        return ((alloc + 31) >> 4) << 4;
    } else if (sizeof(void*) == 4) {
        return ((alloc + 15) >> 3) << 3;
    } else {
        assert(0);
    }
}

// STL data structures

template<typename X>
struct stl_tree_node
{
private:
    int color;
    void* parent;
    void* left;
    void* right;
    X x;
};

struct stl_shared_counter
{
    /* Various platforms use different sized counters here.
     * Conservatively assume that they won't be larger than size_t. */
    void* class_type;
    size_t use_count;
    size_t weak_count;
};

template<typename X>
static inline size_t DynamicUsage(const std::vector<X>& v)
{
    return MallocUsage(v.capacity() * sizeof(X));
}

template<unsigned int N, typename X, typename S, typename D>
static inline size_t DynamicUsage(const prevector<N, X, S, D>& v)
{
    return MallocUsage(v.allocated_memory());
}

template<typename X, typename Y>
static inline size_t DynamicUsage(const std::set<X, Y>& s)
{
    return MallocUsage(sizeof(stl_tree_node<X>)) * s.size();
}

template<typename X, typename Y>
static inline size_t IncrementalDynamicUsage(const std::set<X, Y>& s)
{
    return MallocUsage(sizeof(stl_tree_node<X>));
}

template<typename X, typename Y, typename Z>
static inline size_t DynamicUsage(const std::map<X, Y, Z>& m)
{
    return MallocUsage(sizeof(stl_tree_node<std::pair<const X, Y> >)) * m.size();
}

template<typename X, typename Y, typename Z>
static inline size_t IncrementalDynamicUsage(const std::map<X, Y, Z>& m)
{
    return MallocUsage(sizeof(stl_tree_node<std::pair<const X, Y> >));
}

// indirectmap has underlying map with pointer as key

template<typename X, typename Y>
static inline size_t DynamicUsage(const indirectmap<X, Y>& m)
{
    return MallocUsage(sizeof(stl_tree_node<std::pair<const X*, Y> >)) * m.size();
}

template<typename X, typename Y>
static inline size_t IncrementalDynamicUsage(const indirectmap<X, Y>& m)
{
    return MallocUsage(sizeof(stl_tree_node<std::pair<const X*, Y> >));
}

template<typename X>
static inline size_t DynamicUsage(const std::unique_ptr<X>& p)
{
    return p ? MallocUsage(sizeof(X)) : 0;
}

template<typename X>
static inline size_t DynamicUsage(const std::shared_ptr<X>& p)
{
    // A shared_ptr can either use a single continuous memory block for both
    // the counter and the storage (when using std::make_shared), or separate.
    // We can't observe the difference, however, so assume the worst.
    return p ? MallocUsage(sizeof(X)) + MallocUsage(sizeof(stl_shared_counter)) : 0;
}

template <typename T>
struct NodeSize {
};

template <typename Key, typename V, typename Hash, typename Equals, typename Allocator>
struct NodeSize<std::unordered_map<Key, V, Hash, Equals, Allocator>> {
    [[nodiscard]] static constexpr size_t Value()
    {
        // libstdc++, libc++, and MSVC implement the nodes differently. To get the correct size
        // with the correct alignment, we can simulate that with accordingly nested std::pairs.
        using ValueType = std::pair<const Key, V>;

#if defined(_MSC_VER)
        // list node contains 2 pointers and no hash; see
        // https://github.com/microsoft/STL/blob/main/stl/inc/unordered_map and
        // https://github.com/microsoft/STL/blob/main/stl/inc/list
        return sizeof(std::pair<std::pair<void*, void*>, ValueType>);
#else

#if defined(_LIBCPP_VERSION) // defined in any C++ header from libc++
        // libc++ always stores hash and pointer in the node
        // see https://github.com/llvm/llvm-project/blob/release/13.x/libcxx/include/__hash_table#L92
        return sizeof(std::pair<ValueType, std::pair<size_t, void*>>);
#else
        // libstdc++ doesn't store hash when its operator() is noexcept;
        // see hashtable_policy.h, struct _Hash_node
        // https://gcc.gnu.org/onlinedocs/libstdc++/latest-doxygen/a05689.html
        if (noexcept(std::declval<Hash>()(std::declval<const Key&>()))) {
            return sizeof(std::pair<void*, ValueType>);
        } else {
            // hash is stored along ValueType, and that is wrapped with the pointer.
            return sizeof(std::pair<void*, std::pair<ValueType, size_t>>);
        }
#endif
#endif
    }
};

template<typename X, typename Y>
static inline size_t DynamicUsage(const std::unordered_set<X, Y>& s)
{
    return MallocUsage(sizeof(std::pair<X, void*>)) * s.size() + MallocUsage(sizeof(void*) * s.bucket_count());
}

template <typename Key, typename Value, typename Hash, typename Equals, typename Alloc>
static inline size_t DynamicUsage(const std::unordered_map<Key, Value, Hash, Equals, Alloc>& m)
{
    if constexpr (std::is_same_v<Alloc, node_allocator::Allocator<std::pair<const Key, Value>>>) {
        // Assumes that DynamicUsage of the MemoryResource is called separately. We don't do it here
        // because multiple maps could use the same MemoryResource.
        return MallocUsage(sizeof(void*) * m.bucket_count());
    } else {
        auto node_size = NodeSize<std::unordered_map<Key, Value, Hash, Equals, Alloc>>::Value();
        return MallocUsage(node_size) * m.size() + MallocUsage(sizeof(void*) * m.bucket_count());
    }
}

}

#endif // BITCOIN_MEMUSAGE_H
