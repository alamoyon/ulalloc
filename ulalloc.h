/////////////////////////////
#ifndef ULALLOC_H
#define ULALLOC_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
void* ulalloc(size_t size);
void ulfree(void* ptr);
void* ulrealloc(void* ptr, size_t size);
#ifdef __cplusplus
}
#endif

#endif // ULALLOC_H

/////////////////////////////
#ifdef ULALLOC_IMPLEMENTATION

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
  #include <intrin.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <pthread.h>
#endif

#ifdef ULALLOC_TEST
  static bool ul_g_test_crashed = false;
  static bool ul_g_test_noop = false;
#endif // ULALLOC_TEST

#define UL_MAX_ALLOC (SIZE_MAX / 2)              // Should be plenty
#define UL_MIN_SIZE (32)                         // Mimimum UL_MIN_SIZE for storing the linked list node + header + footer data when freed
#define UL_MIN_SIZE_LOG2 (5)                     // log2(UL_MIN_SIZE); keep in sync with UL_MIN_SIZE
#define UL_PAGE_SIZE (4096)                      // 4KiB Hard coded
#define UL_MIN_ALLOC (UL_PAGE_SIZE * 16)         // 64KiB
#define UL_MAX_ARENA_CHUNK (UL_MIN_ALLOC - sizeof(size_t) * 2) // Biggest chunk living inside a shared arena (two sentinels subtracted). Anything bigger is its own mapping
#define UL_PAYLOAD_ALIGN (16)                    // 16 byte payload alignment
#define UL_NUM_BUCKETS (11)                      // from 32B to 32KiB+, each bucket doubling
#define UL_BEEN_FREED_MASK (1)                   // AND for read, OR to write 1, INVERT mask + AND to write 0
#define UL_PREV_FREE_MASK (1 << 1)               // Same logic as above ^
#define UL_RESERVED_BITS (2)                     // For been_freed and prev_free flags
#ifndef MAP_ANONYMOUS
  #define MAP_ANONYMOUS MAP_ANON                 // Fallback for strict -std=c11 builds
#endif

#define UL_ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))
#ifdef _WIN32
  static SRWLOCK ul_lock = SRWLOCK_INIT;
  #define UL_LOCK()   AcquireSRWLockExclusive(&ul_lock)
  #define UL_UNLOCK() ReleaseSRWLockExclusive(&ul_lock)
#else
  static pthread_mutex_t ul_lock = PTHREAD_MUTEX_INITIALIZER;
  #define UL_LOCK()   pthread_mutex_lock(&ul_lock)
  #define UL_UNLOCK() pthread_mutex_unlock(&ul_lock)
#endif

// Implementation defintions because some of the other internal function calls them
static inline void* ulalloc_impl(size_t size);
static inline void  ulfree_impl(void* ptr);
static inline void* ulrealloc_impl(void* ptr, size_t size);

typedef struct ULFreeNode {
  struct ULFreeNode* next;
  struct ULFreeNode* prev;
} ULFreeNode;

// Intializing the FreeNodes directly inside the ulseglist
static ULFreeNode ulseglist[UL_NUM_BUCKETS] = {
  { &ulseglist[0],  &ulseglist[0]  }, // 32B
  { &ulseglist[1],  &ulseglist[1]  }, // 64B
  { &ulseglist[2],  &ulseglist[2]  }, // 128B
  { &ulseglist[3],  &ulseglist[3]  }, // 256B
  { &ulseglist[4],  &ulseglist[4]  }, // 512B
  { &ulseglist[5],  &ulseglist[5]  }, // 1KiB
  { &ulseglist[6],  &ulseglist[6]  }, // 2KiB
  { &ulseglist[7],  &ulseglist[7]  }, // 4KiB
  { &ulseglist[8],  &ulseglist[8]  }, // 8KiB
  { &ulseglist[9],  &ulseglist[9]  }, // 16KiB
  { &ulseglist[10], &ulseglist[10] }, // 32KiB
};

static inline void ul_list_insert(ULFreeNode* sentinel, ULFreeNode* node) {
  node->next = sentinel->next;
  node->prev = sentinel;
  sentinel->next->prev = node;
  sentinel->next = node;
}

static inline void ul_list_remove(ULFreeNode* node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
}

#ifdef _WIN32
  static inline int ul_get_bucket_index(size_t size) {
    if (size <= UL_MIN_SIZE) return 0;

    unsigned long msb; // Index of the highest set bit, as in floor(log2(size-1))
    _BitScanReverse64(&msb, (unsigned __int64)(size - 1)); // size - 1 is never 0 here since size > UL_MIN_SIZE
    int bits = (int)msb + 1; // Same as 64 - __builtin_clzll(size - 1)
    int index = bits - UL_MIN_SIZE_LOG2; // Offset so 32B (2^5) maps to index 0
    index = (index < UL_NUM_BUCKETS) ? index : (UL_NUM_BUCKETS - 1);
    assert(index >= 0 && index < UL_NUM_BUCKETS);

    return index;
  }
#else
  static inline int ul_get_bucket_index(size_t size) {
    if (size <= UL_MIN_SIZE) return 0;

    int bits = 64 - __builtin_clzll(size - 1); // Get the log2(size-1)
    int index = bits - UL_MIN_SIZE_LOG2; // Offset so 32B (2^5) maps to index 0
    index = (index < UL_NUM_BUCKETS) ? index : (UL_NUM_BUCKETS - 1);
    assert(index >= 0 && index < UL_NUM_BUCKETS);

    return index;
  }
#endif

static inline size_t* ul_get_freed_header_from_seglist(size_t size) {
  int index = ul_get_bucket_index(size);

  for (size_t i = index; i < UL_NUM_BUCKETS; ++i) {
    ULFreeNode* sentinel = &ulseglist[i];
    for (ULFreeNode* curr = sentinel->next; curr != sentinel; curr = curr->next) {
      size_t* header = (size_t*)curr - 1;
      if (((*header) >> UL_RESERVED_BITS) >= size) {
        ul_list_remove(curr);
        return header; 
      }
    }
  }
  return NULL;
}

static inline size_t* ul_mark_sentinel(size_t* ptr, size_t size) {
  if (ptr == NULL) return NULL;

  size_t prologue = 0; // Every flags are off
  size_t epilogue = 2; // (...0010) enable prev_free flag bit

  memcpy(ptr, &prologue, sizeof(size_t));
  memcpy((char*)ptr + size - sizeof(size_t), &epilogue, sizeof(size_t));
 
  return ptr + 1; 
}

#ifdef _WIN32
  static inline size_t* ul_expand_heap(size_t size) {
    void* ptr = VirtualAlloc(
      NULL,          // Let the OS decide the address
      size,          // Size to expand
      MEM_RESERVE | MEM_COMMIT, // Both reserve and use the memory
      PAGE_READWRITE // Read-write permission
    );

    if (ptr == NULL) return NULL; // Running out of memory is a legitimate runtime failure, not an invariant violation
    return ul_mark_sentinel((size_t*)ptr, size);
  }
#else 
  static inline size_t* ul_expand_heap(size_t size) {
    void* ptr = mmap(
      NULL,         // Let the OS decide the address
      size,         // Size to expand
      PROT_READ | PROT_WRITE, // Read-write permission
      MAP_PRIVATE | MAP_ANONYMOUS, // No shared memory when forked | Memory not pegged to a file
      -1, 0         // Therfore no file descriptor and offest needed 
    );

    if (ptr == MAP_FAILED) return NULL; // Running out of memory is a legitimate runtime failure, not an invariant violation
    return ul_mark_sentinel((size_t*)ptr, size);
  }
#endif

static inline void ul_prepare_and_put_remaining_chunk_in_seglist(size_t* ptr, size_t size) {
  size_t header = size << UL_RESERVED_BITS; // prev_free off by default
  header |= UL_BEEN_FREED_MASK; // on
  ptr[0] = header;
  memcpy((char*)ptr + size - sizeof(size_t), &size, sizeof(size_t)); // footer at the end
  assert((size_t)(ptr + 1) % UL_PAYLOAD_ALIGN == 0); // Paylad should be 16 byte aligned
  
  // Put the free chunk into ulseglist
  int index = ul_get_bucket_index(size);
  ul_list_insert(&ulseglist[index], (ULFreeNode*)(ptr + 1));

  // Mark prev_free for the next chunk header
  *(size_t*)((char*)ptr + size) |= UL_PREV_FREE_MASK;
}

static inline void* ul_prepare_primary_chunk(size_t* header_ptr, size_t size) {
  size_t header = size << UL_RESERVED_BITS; // It's not free, and logically can't have prev_free flag on
  header_ptr[0] = header;
  assert((size_t)(header_ptr + 1) % UL_PAYLOAD_ALIGN == 0);

  // Mark prev free flag of next header
  size_t* next_header = (size_t*)((char*)header_ptr + size);
  *next_header &= ~UL_PREV_FREE_MASK;

  return header_ptr + 1;
}

static inline bool ul_chunk_is_free(void* chunk) {
  assert(chunk != NULL);
  size_t header = *((size_t*)chunk - 1);
  return header & UL_BEEN_FREED_MASK;
}

#ifdef _WIN32
  static inline int ul_shrink_heap(void* ptr, size_t size) {
    (void)size; // MEM_RELEASE requires the size to be 0 and releases the whole region that VirtualAlloc returned
    int result = VirtualFree(ptr, 0, MEM_RELEASE);
    return result ? 0 : -1; // Returns nonzero when passes; 0 when fails
  } 
#else
  static inline int ul_shrink_heap(void* ptr, size_t size) {
    return munmap(ptr, size); // Returns 0 when passes; -1 when fails
  }
#endif

static inline void* ul_allocate_copy_and_free(void* prev_header, size_t prev_size, size_t new_size) {
  void* new_chunk = ulalloc_impl(new_size);
  if (!new_chunk) return NULL; // The original chunk stays intact, just like realloc
  size_t prev_payload = prev_size - sizeof(size_t);
  memcpy(new_chunk, prev_header, prev_payload < new_size ? prev_payload : new_size); // Never copy more than the new chunk can hold
  ulfree_impl(prev_header);

  return new_chunk;
}

static inline void ul_crash_program(void* ptr, const char* msg) {
  #ifdef ULALLOC_TEST
    (void)ptr;
    (void)msg;
    ul_g_test_crashed = true;
    return;
  #else
    fprintf(stderr, "Fatal: %s: %p!\n", msg, ptr);
    exit(EXIT_FAILURE);
  #endif
}

static inline void* ulalloc_impl(size_t size) {
  if (!size || size > UL_MAX_ALLOC) return NULL; 

  // Calculate extra bytes for storing (size_t) header in the allocated memory. 
  size += sizeof(size_t); 
  size = size < UL_MIN_SIZE ? UL_MIN_SIZE : size; // To contain all the metadata when freed, a chunk at minimum needs to be UL_MIN_SIZE
  size = UL_ALIGN_UP(size, UL_PAYLOAD_ALIGN);
  size_t chunk_size = size;

  // Look for memory inside the ulseglist first. 
  size_t* header_ptr = ul_get_freed_header_from_seglist(size); // returns pointer to header if available, else NULL

  // If not found in ulseglist, beg the OS for more memory
  if (header_ptr == NULL) {
    chunk_size += sizeof(size_t) * 2;     // extra sizeof(size_t) * 2 bytes for the sentinels
    chunk_size = chunk_size <= UL_MIN_ALLOC ? UL_MIN_ALLOC : UL_ALIGN_UP(chunk_size, UL_PAGE_SIZE); // Make sure the req_size is a multiple of 4KiB 
    header_ptr = ul_expand_heap(chunk_size); // expadn_heap already offsets the header_ptr past the prologue (left sentinel)

    if (header_ptr == NULL) return NULL;  // Neither the ulseglist nor the os has enough memory to lend; return NULL
    assert((size_t)header_ptr % 8 == 0 && (size_t)header_ptr % UL_PAYLOAD_ALIGN != 0); // header location should not be a multiple of PAYLOAD_ALIGN
    chunk_size -= sizeof(size_t) * 2;     // Remove the extra bytes taken up by the sentinels from the usuable chunk
  } else { // Extract the size from the header
    chunk_size = (*header_ptr) >> UL_RESERVED_BITS;
  }

  // Take up the necessary padding spaces and keep the extra bytes if they are less than UL_MIN_SIZE
  size_t remaining = chunk_size - size; // remaining bytes. Should logically never be < 0
  if (remaining >= UL_MIN_SIZE && chunk_size <= UL_MAX_ARENA_CHUNK) { // Standalone (large) mappings are never split
    size_t* remaining_header_ptr = (size_t*)((char*)header_ptr + size);
    ul_prepare_and_put_remaining_chunk_in_seglist(remaining_header_ptr, remaining);
    chunk_size -= remaining;
  }

  return ul_prepare_primary_chunk(header_ptr, chunk_size);
}

static inline void ulfree_impl(void* ptr) {
  if (ptr == NULL) {
    #ifdef ULALLOC_TEST
      ul_g_test_noop = true;
    #endif
    return;
  }
  if (ul_chunk_is_free(ptr)) {
    ul_crash_program(ptr, "Attempted double free on pointer");
    return;
  }

  size_t* header_ptr = (size_t*)ptr - 1;
  size_t header = *header_ptr;
  size_t size = header >> UL_RESERVED_BITS;
  size_t* next_header_ptr = (size_t*)((char*)header_ptr + size);
  size_t next_header = *next_header_ptr;

  if (size > UL_MAX_ARENA_CHUNK) { // Standalone mapping; it was never put in an arena
    ul_shrink_heap(
      header_ptr - 1,           // Pointer to prologue (left sentinel) 
      size + sizeof(size_t) * 2 // +sizeof(size_t) for both sentinels
    );
    return;
  }

  // Merge right neighbour if free
  if (next_header & UL_BEEN_FREED_MASK) {
    size += next_header >> UL_RESERVED_BITS;
    ul_list_remove((ULFreeNode*)(next_header_ptr + 1));
  }

  // Merge left neighbour if free
  if (header & UL_PREV_FREE_MASK) {
    size_t* prev_footer_ptr = header_ptr - 1;
    size_t prev_size = *prev_footer_ptr;
    header_ptr = (size_t*)((char*)header_ptr - prev_size);
    ul_list_remove((ULFreeNode*)(header_ptr + 1));
    size += prev_size;
  }

  // Shrink heap if the whole arena has been freed
  if (size == UL_MAX_ARENA_CHUNK) {
    ul_shrink_heap(header_ptr - 1, UL_MIN_ALLOC);  // Pointer to prologue (left sentinel) 
    return;
  }

  // Prepare new header
  header = size << UL_RESERVED_BITS; // prev_free off by default.
  header |= UL_BEEN_FREED_MASK; // on
  
  // Set header and footer
  header_ptr[0] = header;
  memcpy((char*)header_ptr + size - sizeof(size_t), &size, sizeof(size_t));

  // Insert to ulseglist
  int index = ul_get_bucket_index(size);
  ULFreeNode* node = (ULFreeNode*)(header_ptr + 1);
  ul_list_insert(&ulseglist[index], node);

  // Mark prev_free for the next chunk
  size_t* new_next_header_ptr = (size_t*)((char*)header_ptr + size);
  *new_next_header_ptr |= UL_PREV_FREE_MASK;
} 

static inline void* ulrealloc_impl(void* ptr, size_t size) {
  if (!size) {                  // If size is 0, free the existing pointer and return NULL 
    if (ptr) ulfree_impl(ptr);  // It's an arbitrary implementation-depended behaviour, not defined in C standards
    return NULL;                // Although Linux and Windows follow this covention
  }
  if (size > UL_MAX_ALLOC) return NULL; // What kind of program needs that much memory!?
  if (!ptr) return ulalloc_impl(size);  // Allocate a new chunk
  if (ul_chunk_is_free(ptr)) { 
    ul_crash_program(ptr, "ulrealloc called on freed pointer");
    return NULL;
  }

  // Extract current chunk data
  size_t* curr_header_ptr = (size_t*)ptr - 1;
  size_t curr_header = *curr_header_ptr;
  size_t curr_size = curr_header >> UL_RESERVED_BITS;

  size_t og_size = size;
  size += sizeof(size_t); 
  size = size < UL_MIN_SIZE ? UL_MIN_SIZE : size; 
  size = UL_ALIGN_UP(size, UL_PAYLOAD_ALIGN);

  // Standalone (large) mappings on either side can't be split or merged in place; move the data instead
  if (curr_size > UL_MAX_ARENA_CHUNK || size > UL_MAX_ARENA_CHUNK) {
    return ul_allocate_copy_and_free(ptr, curr_size, og_size);
  }

  // New chunk size and ptr if merging is possible
  size_t* new_header_ptr = curr_header_ptr;
  size_t merged_size = curr_size;
  size_t* next_header_ptr = (size_t*)((char*)curr_header_ptr + curr_size);
  size_t next_header = *next_header_ptr;
  size_t* prev_header_ptr = NULL;

  if (next_header & UL_BEEN_FREED_MASK) {
    merged_size += next_header >> UL_RESERVED_BITS;
  } else {
    next_header_ptr = NULL;
  }

  if (curr_header & UL_PREV_FREE_MASK) {
    size_t prev_size = *(curr_header_ptr - 1);
    prev_header_ptr = (size_t*)((char*)curr_header_ptr - prev_size);
    new_header_ptr = prev_header_ptr;
    merged_size += (*prev_header_ptr) >> UL_RESERVED_BITS;
  }

  // If the merged size is still not enough, return a new chunk
  if (merged_size < size) return ul_allocate_copy_and_free(ptr, curr_size, og_size);

  // Rmeove the neighbours from the ulseglist
  if (prev_header_ptr) {
    ul_list_remove((ULFreeNode*)(prev_header_ptr + 1));
  }
  if (next_header_ptr) {
    ul_list_remove((ULFreeNode*)(next_header_ptr + 1));
  }

  // Calculate remaining size (relative to the requested size) and subtract it from merged_size
  size_t remaining = merged_size - size;
  remaining = remaining >= UL_MIN_SIZE ? remaining : 0;
  merged_size -= remaining;

  // Copy over the data from previous place
  size_t payload = (curr_size < merged_size ? curr_size : merged_size) - sizeof(size_t);
  memmove(new_header_ptr + 1, ptr, payload);

  // Put extra bytes in the ulseglist if available (after the move, so the remainder can't clobber unmoved data)
  if (remaining) {
    size_t* remaining_header_ptr = (size_t*)((char*)new_header_ptr + merged_size);
    ul_prepare_and_put_remaining_chunk_in_seglist(remaining_header_ptr, remaining);
  } 

  return ul_prepare_primary_chunk(new_header_ptr, merged_size);
} 

void* ulalloc(size_t size) {
  UL_LOCK();
  void* chunk = ulalloc_impl(size);
  UL_UNLOCK();
  return chunk;
}
void ulfree(void* ptr) {
  UL_LOCK();
  ulfree_impl(ptr);
  UL_UNLOCK();
}
void* ulrealloc(void* ptr, size_t size) {
  UL_LOCK();
  void* chunk = ulrealloc_impl(ptr, size);
  UL_UNLOCK();
  return chunk;
}

#endif // ULALLOC_IMPLEMENTATION
