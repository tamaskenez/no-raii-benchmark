
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <deque>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

using std::array;
using std::deque;
using std::make_unique;
using std::move;
using std::unique_ptr;
using std::vector;

using hrclock = std::chrono::high_resolution_clock;
using ddur = std::chrono::duration<double>;
using time_point = hrclock::time_point;
using duration = hrclock::duration;

const int N_CHILDREN = 3;
const int TOTAL_LEVELS = 12;

struct Counters {
  int nodes = 0;
  size_t alloc_size = 0;
  int allocs = 0;
  int frees = 0;
};

Counters counters;

void *operator new(size_t size) {
  ++counters.allocs;
  counters.alloc_size += size;
  void *p = malloc(size);
  return p;
}

void *my_new(size_t size) {
  ++counters.allocs;
  counters.alloc_size += size;
  void *p = malloc(size);
  return p;
}

void operator delete(void *p) noexcept {
  ++counters.frees;
  free(p);
}

void my_delete(void *p) noexcept {
  ++counters.frees;
  free(p);
}

template <class T> struct aligned_item_size {
  static constexpr int value =
      ((sizeof(T) + alignof(T) - 1) / alignof(T)) * alignof(T);
};

namespace usual {

struct Allocator {};

struct Node {
  vector<unique_ptr<Node>> children;
  int num = counters.nodes++;

  Node(Allocator &, int n_max_children) { children.reserve(n_max_children); }
  void add_child(Allocator &a, int n_max_children) {
    children.emplace_back(make_unique<Node>(a, n_max_children));
  }
};

} // namespace usual

namespace optim {

const auto MAX_ALIGN = sizeof(std::max_align_t);

class SingleBlock {
  unique_ptr<std::max_align_t[]> block;

public:
  explicit SingleBlock(size_t N)
      : block(
            make_unique<std::max_align_t[]>((N + MAX_ALIGN - 1) / MAX_ALIGN)) {}

  void *get() const { return block.get(); }
};

const int MAX_SMALL_BLOCK_SIZE = 4096;
const int BLOCK_PAGE_SIZE = 65536;

class Allocator {
  deque<void *> pages;
  deque<SingleBlock> big_blocks;
  void *active_page_free_begin = nullptr;
  size_t active_page_free_bytes = 0;

public:
  void *allocate_block(size_t size, size_t alignment) {
    if (size <= MAX_SMALL_BLOCK_SIZE) {
      if (!active_page_free_begin) {
        active_page_free_begin = my_new(BLOCK_PAGE_SIZE);
        active_page_free_bytes = BLOCK_PAGE_SIZE;
        pages.push_back(active_page_free_begin);
      }
      if (std::align(alignment, size, active_page_free_begin,
                     active_page_free_bytes)) {
        auto result = active_page_free_begin;
        active_page_free_begin = (uint8_t *)active_page_free_begin + size;
        active_page_free_bytes -= size;
        return result;
      } else {
        active_page_free_begin = nullptr;
        active_page_free_bytes = 0;
        return allocate_block(size, alignment);
      }
    } else {
      big_blocks.emplace_back(size);
      return big_blocks.back().get();
    }
  }
  ~Allocator() {
    for (auto p : pages)
      my_delete(p);
  }

  template <class T, class... Args> T *new_object(Args &&... args) {
    void *p = allocate_block(sizeof(T), alignof(T));
    return new (p) T(std::forward<Args>(args)...);
  }
};

template <class T> class myvector {
  T *items;
  int size = 0;
  const int max_size;

public:
  explicit myvector(Allocator &a, int max_size)
      : items((T *)(a.allocate_block(max_size * aligned_item_size<T>::value,
                                     alignof(T)))),
        max_size(max_size) {}
  void push_back(const T &x) {
    assert(size < max_size);
    new (&(items[size++])) T(x);
  }
  T *begin() { return items; }
  T *end() { return items + size; }
  const T *begin() const { return items; }
  const T *end() const { return items + size; }
};

struct Node {
  myvector<Node *> children;
  int num = counters.nodes++;

  Node(Allocator &a, int n_max_children) : children(a, n_max_children) {}
  void add_child(Allocator &a, int n_max_children) {
    Node *p = a.new_object<Node>(a, n_max_children);
    children.push_back(p);
  }
};
} // namespace optim

namespace alloc {

struct Allocator;
Allocator *g_allocator;

struct Allocator : public optim::Allocator {
  Allocator() { g_allocator = this; }
  ~Allocator() { g_allocator = nullptr; }
};

template <typename T> class MyStdAllocator {
public:
  using value_type = T;
  using size_type = std::size_t;
  using propagate_on_container_move_assignment = std::true_type;

  MyStdAllocator() noexcept = default;
  MyStdAllocator(const MyStdAllocator &) noexcept {}
  template <typename U> MyStdAllocator(const MyStdAllocator<U> &) noexcept {}
  MyStdAllocator(MyStdAllocator &&other) noexcept {}
  MyStdAllocator &operator=(const MyStdAllocator &) noexcept { return *this; }
  MyStdAllocator &operator=(MyStdAllocator &&other) noexcept { return *this; }
  ~MyStdAllocator() noexcept = default;

  T *allocate(size_type n) {
    return (T *)g_allocator->allocate_block(n * aligned_item_size<T>::value,
                                            alignof(T));
  }
  void deallocate(T *ptr, size_type n) noexcept {}
};

struct Node {
  vector<Node *, MyStdAllocator<Node *>> children;
  int num = counters.nodes++;

  Node(Allocator &, int n_max_children) { children.reserve(n_max_children); }
  void add_child(Allocator &a, int n_max_children) {
    Node *p = a.new_object<Node>(a, n_max_children);
    children.push_back(p);
  }
};

} // namespace alloc

template <class Allocator, class Node>
void build_subtree(Allocator &allocator, Node &node, int levels_left) {
  for (int i = 0; i < N_CHILDREN; ++i)
    node.add_child(allocator, N_CHILDREN);
  if (--levels_left <= 0) {
    return;
  }
  for (auto &c : node.children) {
    build_subtree(allocator, *c, levels_left);
  }
}

template <class Node> int traverse(const Node &node) {
  int result = node.num;
  for (auto &c : node.children) {
    result += traverse(*c);
  }
  return result;
}

template <class Allocator, class Node> Node build_tree(Allocator &allocator) {
  Node root(allocator, N_CHILDREN);
  build_subtree(allocator, root, TOTAL_LEVELS);
  return root;
}

template <class Allocator, class Node> void test(const char *name) {
  counters = Counters{};
  fprintf(stderr, "-- Testing: %s\n", name);
  time_point t0, t1;
  duration dur_build, dur_traverse, dur_dtor;
  {
    Allocator allocator;
    t0 = hrclock::now();
    auto r = build_tree<Allocator, Node>(allocator);
    t1 = hrclock::now();
    dur_build = t1 - t0;
    fprintf(stderr, "Node count: %d, build time: %.3f ms\n", counters.nodes,
            1000.0 * ddur(dur_build).count());
    t0 = hrclock::now();
    int n = traverse(r);
    t1 = hrclock::now();
    dur_traverse = t1 - t0;
    fprintf(stderr, "traverse result: %d, time: %.3f ms\n", n,
            1000.0 * ddur(dur_traverse).count());
    t0 = hrclock::now();
  }
  t1 = hrclock::now();
  dur_dtor = t1 - t0;
  fprintf(stderr, "Destruction: %.3f ms\n", 1000.0 * ddur(dur_dtor).count());
  fprintf(stderr, "Total time: %.3f ms\n",
          1000.0 * ddur(dur_build + dur_traverse + dur_dtor).count());
  fprintf(stderr, "%d allocations (%.3f MB), %d frees.\n", counters.allocs,
          counters.alloc_size / 1e6, counters.frees);
}

int main() {
  test<optim::Allocator, optim::Node>("Optim");
  test<usual::Allocator, usual::Node>("Usual");
  test<alloc::Allocator, alloc::Node>("Alloc");
}
