# ulalloc

*Pronounced "ul-alloc", short for "Useless allocator".*

A thread-safe, cross-platform memory allocator written in C to understand how
allocators work. It has no practical purpose: use the C standard library's
`malloc` instead.

## Design
- **Segregated free lists (seglists):** freed chunks are grouped into 11
  power-of-two size buckets, from 32B to 32KiB+.
- **Boundary-tag coalescing:** adjacent free chunks are merged on `free`.
- **Compact metadata:** the `been_freed` and `prev_free` flags live in the two
  low bits of the 8-byte (`size_t`) header. The footer is only written when a
  chunk is freed.
- **OS-backed arenas:** when no free chunk fits, `mmap` (or `VirtualAlloc` on
  Windows) provides a fresh arena of at least 64KiB. The unused remainder is
  split off and put into the seglists. Sentinels at both ends of an arena stop
  merging from running past its edges.
- **Large requests:** requests that don't fit in a 64KiB arena are rounded up
  to a page multiple and mapped on their own. They are never split or placed
  in the seglists, and they are unmapped on `free`.
- **Thread safety:** one global lock around `ulalloc`, `ulfree` and `ulrealloc`.
