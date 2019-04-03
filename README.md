# Benchmarking RAII vs region-based allocation strategies.

This small test app builds, traverses and deallocates a simple tree using two different allocation strategies:

1. The usual, RAII-style allocation where each node of the tree holds a vector of owning pointers to the children:

    struct Node {
        vector<unique_ptr<Node>> children;
    }
  
2. A so-called region-based allocation scheme where the small blocks of memory for the nodes and vectors are allocated from a local pool. The pool itself allocates its memory supply from the heap in bigger blocks. The memory is deallocated at once only when the pool object goes out of scope:

    struct Node {
        pool_based_vector<Node*> children; // pool_based_vector's dtor does nothing.
    }
    
    ...
    
    {
        Pool pool;
        {
          auto tree = build_tree(..., pool); // Allocates small blocks from pool
          update_tree(); // Maybe removing nodes, memory for removed nodes is not released here.
        } // <- tree goes out of scope, nothing is released.
    } // <- pool goes out of scope, all memory returned to heap

## Results:
(MacBook 2.2 GHz Intel Core i7)

Tree node count: 21523360 (15 levels, 3 children/node)

|                   |         RAII    |   Region
|-------------------|-----------------|-----------------
|        Build time:|  3.169s ( 343%) |  0.923s (100%)
|    Traversal time:|  0.182s ( 116%) |  0.157s (100%)
| Deallocation time:|  3.169s (4972%) |  0.073s (100%)
|        Total time:|  6.972s ( 605%) |  1.153s (100%)
|  Heap allocations:|      43046719   |          997
|Heap deallocations:|      43046719   |          997
|   Bytes allocated:|      1205.308MB |     1033.912MB

Region-based memory management can be simulated in C++. One can even use std containers without overhead using user-defined allocators. Probably it gets ugly pretty quickly.

    {
        Pool pool;
        vector<int, Pool> v;
        ...
    }
    
More info: https://en.wikipedia.org/wiki/Region-based_memory_management
