#include <cassert>
#include <chrono>
#include <cstddef>
#include <deque>
#include <memory>
#include <vector>

using std::deque;
using std::make_unique;
using std::unique_ptr;
using std::vector;

const int N_CHILDREN = 3;  // Each non-leaf node has this many children.
const int TREE_DEPTH = 15;

// Global new and delete operators redefined to logging versions.

struct AllocationStat
{
    int n_nodes_created = 0;
    size_t total_bytes_allocated = 0;
    int n_allocations = 0;
    int n_frees = 0;
};

AllocationStat g_stat;

inline void* logging_new(size_t size)
{
    ++g_stat.n_allocations;
    g_stat.total_bytes_allocated += size;
    void* p = malloc(size);
    return p;
}

void* operator new(size_t size)
{
    return logging_new(size);
}

inline void logging_delete(void* p) noexcept
{
    ++g_stat.n_frees;
    free(p);
}

void operator delete(void* p) noexcept
{
    logging_delete(p);
}

namespace with_raii {

// Allocator is not used in this implementation. It's here only to ensure this Node has the same API
// as the other implementation.
struct Allocator
{};

// Standard, RAII-style tree node.
struct Node
{
    vector<unique_ptr<Node>> children;
    const int node_id = g_stat.n_nodes_created++;

    Node(Allocator&, int max_n_children) { children.reserve(max_n_children); }
    void add_child(Allocator& a, int max_n_children)
    {
        children.push_back(make_unique<Node>(a, max_n_children));
    }
};

}  // namespace with_raii

namespace without_raii {

const int MAX_SMALL_BLOCK_SIZE = 4096;
const int PAGE_SIZE = 65536;

// Region-style allocator which allocates small blocks from bigger pages and release everything only
// at a single point, when it goes out of scope.
class Allocator
{
    using Page = std::aligned_storage<PAGE_SIZE, 1024>::type;
    deque<Page> pages;
    void* active_page_first_free_byte = nullptr;
    size_t active_page_bytes_left = 0;

public:
    ~Allocator() = default;  // All the pages are released here.

    void* allocate_block(size_t size, size_t alignment)
    {
        if (size <= MAX_SMALL_BLOCK_SIZE) {
            if (!active_page_first_free_byte) {
                // Need a new page.
                pages.emplace_back();
                active_page_first_free_byte = &pages.back();
                active_page_bytes_left = PAGE_SIZE;
            }
            if (std::align(alignment, size, active_page_first_free_byte, active_page_bytes_left)) {
                // Allocate from active page.
                auto result = active_page_first_free_byte;
                active_page_first_free_byte = (char*)active_page_first_free_byte + size;
                active_page_bytes_left -= size;
                return result;
            } else {
                // No room in active page, inactivate and retry.
                active_page_first_free_byte = nullptr;
                active_page_bytes_left = 0;
                return allocate_block(size, alignment);
            }
        }
        fprintf(stderr, "Allocating more then %d is not implemented.\n", MAX_SMALL_BLOCK_SIZE);
        std::terminate();
    }

    // Allocate and placement-new.
    template <class T, class... Args>
    T* new_object(Args&&... args)
    {
        void* p = allocate_block(sizeof(T), alignof(T));
        return new (p) T(std::forward<Args>(args)...);
    }
};

// Round-up sizeof(T) to alignment.
template <class T>
struct aligned_item_size
{
    static constexpr int value = ((sizeof(T) + alignof(T) - 1) / alignof(T)) * alignof(T);
};

// Vector which allocates fixed memory in constructor, from the region-style allocator (and no
// deallocation).
template <class T>
class Vector
{
    T* const items;
    int size = 0;
    const int max_size;

public:
    Vector(Allocator& a, int max_size)
        : items((T*)(a.allocate_block(max_size * aligned_item_size<T>::value, alignof(T)))),
          max_size(max_size)
    {}

    // Placement-new and increase size.
    void push_back(const T& x)
    {
        assert(size < max_size);
        new (&(items[size++])) T(x);
    }
    const T* begin() const { return items; }
    const T* end() const { return items + size; }
};

// Node, which stores its region-allocated children in the region-allocated Vector.
struct Node
{
    Vector<Node*> children;
    const int node_id = g_stat.n_nodes_created++;

    Node(Allocator& a, int max_n_children) : children(a, max_n_children) {}
    void add_child(Allocator& a, int max_n_children)
    {
        Node* new_node = a.new_object<Node>(a, max_n_children);
        children.push_back(new_node);
    }
};
}  // namespace without_raii

// Build a tree TREE_DEPTH levels deep, each node having N_CHILDREN children.
template <class Allocator, class Node>
void build_subtree(Allocator& allocator, Node& node, int levels_left)
{
    for (int i = 0; i < N_CHILDREN; ++i)
        node.add_child(allocator, N_CHILDREN);
    if (--levels_left <= 0) {
        return;
    }
    for (auto& c : node.children) {
        build_subtree(allocator, *c, levels_left);
    }
}

template <class Allocator, class Node>
Node build_tree(Allocator& allocator)
{
    Node root(allocator, N_CHILDREN);
    build_subtree(allocator, root, TREE_DEPTH);
    return root;
}

// Traverse the tree (depth-first), calculate checksum.
template <class Node>
int traverse(const Node& node)
{
    int checksum = node.node_id;
    for (auto& c : node.children) {
        checksum = (checksum + traverse(*c)) % 43112609;
    }
    return checksum;
}

using hrclock = std::chrono::high_resolution_clock;
using ddur = std::chrono::duration<double>;
using time_point = hrclock::time_point;
using duration = hrclock::duration;

struct Report
{
    duration build, traversal, deallocation;
    int checksum;
    AllocationStat allocations;

    duration total() const { return build + traversal + deallocation; }
};

template <class Allocator, class Node>
Report test(const char* name)
{
    g_stat = AllocationStat{};
    fprintf(stderr, "-- Testing: %s\n", name);
    time_point t0, t1, t2, t3, t4, t5;
    int checksum;
    {
        Allocator allocator;
        t0 = hrclock::now();
        auto r = build_tree<Allocator, Node>(allocator);
        t1 = hrclock::now();
        t2 = hrclock::now();
        checksum = traverse(r);
        t3 = hrclock::now();
        t4 = hrclock::now();
    }
    t5 = hrclock::now();
    return Report{t1 - t0, t3 - t2, t5 - t4, checksum, g_stat};
}

int main()
{
    fprintf(stderr, "Benchmarking the building, traversal and deallocation of a tree using:\n\n");
    fprintf(stderr,
            "1. The usual, RAII-style storage (node holds a vector of owning pointers\n   to the "
            "children)\n");
    fprintf(stderr,
            "2. Region-style allocator: all memory is allocated from a local pool (region) and\n");
    fprintf(stderr, "   deallocated at once when the region goes out of scope.\n\n");
    auto region = test<without_raii::Allocator, without_raii::Node>("Region-allocator");
    auto raii = test<with_raii::Allocator, with_raii::Node>("RAII-allocator");
    fprintf(stderr, "\n");

    if (region.allocations.n_nodes_created != raii.allocations.n_nodes_created ||
        region.checksum != raii.checksum) {
        fprintf(stderr, "Internal error, different checksum or number of nodes created.\n");
        std::terminate();
    }
    fprintf(stderr, "Tree node count: %d (%d levels, %d children/node)\n\n",
            region.allocations.n_nodes_created, TREE_DEPTH, N_CHILDREN);
    fprintf(stderr, "                            RAII    |   Region\n");
    fprintf(stderr, "                    ----------------|-----------------\n");
    auto sec = [](duration d) { return ddur(d).count(); };
    fprintf(stderr, "        Build time: %6.3fs (%4.0f%%) | %6.3fs (100%%)\n", sec(raii.build),
            100.0 * sec(raii.build) / sec(region.build), sec(region.build));
    fprintf(stderr, "    Traversal time: %6.3fs (%4.0f%%) | %6.3fs (100%%)\n", sec(raii.traversal),
            100.0 * sec(raii.traversal) / sec(region.traversal), sec(region.traversal));
    fprintf(stderr, " Deallocation time: %6.3fs (%4.0f%%) | %6.3fs (100%%)\n", sec(raii.build),
            100.0 * sec(raii.deallocation) / sec(region.deallocation), sec(region.deallocation));
    fprintf(stderr, "        Total time: %6.3fs (%4.0f%%) | %6.3fs (100%%)\n", sec(raii.total()),
            100.0 * sec(raii.total()) / sec(region.total()), sec(region.total()));
    fprintf(stderr, "  Heap allocations:  %12d   | %12d\n", raii.allocations.n_allocations,
            region.allocations.n_allocations);
    fprintf(stderr, "Heap deallocations:  %12d   | %12d\n", raii.allocations.n_frees,
            region.allocations.n_frees);
    fprintf(stderr, "   Bytes allocated:  %12.3fMB | %12.3fMB\n",
            raii.allocations.total_bytes_allocated / 1e6,
            region.allocations.total_bytes_allocated / 1e6);
}
