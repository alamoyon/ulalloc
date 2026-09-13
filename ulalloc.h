#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <assert.h>
#include <string.h>
#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
  #include <pthread.h>
#endif
                                 // (...111100) The least siginifcant bit is used for the `been_freed` flag
#define MAX_SIZE ~(size_t)3      // And the second least significant bit is used for the `prev_free` flag
#define MIN_SIZE  32             // Mimimum MIN_SIZE for storing the linked list node + header + footer data when freed
#define PAGE_SIZE 4096           // 4KiB Hard coded
#define MIN_ALLOC PAGE_SIZE * 16 // 64KiB
#define PAYLOAD_ALIGN 16         // 16 byte payload alignment
#define NUM_BUCKETS 11           // from 32B to 32KiB+, each bucket doubling
#define BEEN_FREED_MASK 1        // AND for read, OR to write 1, INVERT mask + AND to write 0
#define PREV_FREE_MASK (1 << 1)  // Same logic as above ^
#define RESERVED_BITS 2          // For been_freed and prev_free flags

#define ALIGN_UP(size, align) (((size) + (align) - 1) & ~((align) - 1))

typedef struct FreeNode {
  struct FreeNode* next;
  struct FreeNode* prev;
} FreeNode;

// Intializing the FreeNodes directly inside the seglist
static FreeNode seglist[NUM_BUCKETS] = {
  { &seglist[0],  &seglist[0]  }, // 32B
  { &seglist[1],  &seglist[1]  }, // 64B
  { &seglist[2],  &seglist[2]  }, // 128B
  { &seglist[3],  &seglist[3]  }, // 256B
  { &seglist[4],  &seglist[4]  }, // 512B
  { &seglist[5],  &seglist[5]  }, // 1KiB
  { &seglist[6],  &seglist[6]  }, // 2KiB
  { &seglist[7],  &seglist[7]  }, // 4KiB
  { &seglist[8],  &seglist[8]  }, // 8KiB
  { &seglist[9],  &seglist[9]  }, // 16KiB
  { &seglist[10], &seglist[10] }, // 32KiB
};

static inline void list_insert(FreeNode* sentinel, FreeNode* node) {
  node->next = sentinel->next;
  node->prev = sentinel;
  sentinel->next->prev = node;
  sentinel->next = node;
}

static inline void list_remove(FreeNode* node) {
  node->prev->next = node->next;
  node->next->prev = node->prev;
}

static inline int get_bucket_index(size_t size) {
  if (size <= 32) return 0;
  
  size_t bits = 64 - __builtin_clzll(size - 1); // Get the log2(size-1)
  size_t index = bits - 5; // Offset so 32B (2^5) maps to index 0
  index = (index < NUM_BUCKETS) ? index : (NUM_BUCKETS - 1);
  assert(index >= 0 && index < NUM_BUCKETS);

  return index;
}

static inline void* get_free_chunk_from_seglist(size_t size) {
  int index = get_bucket_index(size);

  for (size_t i = index; i < NUM_BUCKETS; ++i) {
    FreeNode* sentinel = &seglist[i];
    for (FreeNode* curr = sentinel->next; curr != sentinel; curr = curr->next) {
      size_t* header = (size_t*)curr - 1;
      if (((*header) >> RESERVED_BITS) >= size) {
        list_remove(curr);
        return header; 
      }
    }
  }
  return NULL;
}

static inline void* mark_sentinel(size_t* ptr, size_t size) {
  if (ptr == NULL) return NULL;

  size_t prologue = 0; // Every flags are off
  size_t epilogue = 2; // (...0010) enable prev_free flag bit

  memcpy(ptr, &prologue, sizeof(size_t));
  memcpy((char*)ptr + size - sizeof(size_t), &epilogue, sizeof(size_t));
 
  return ptr + 1; 
}

#ifdef _WIN32
  static inline void* expand_heap(size_t size) {
    void* ptr = VirtualAlloc(
      NULL,          // Let the OS decide the address
      size,          // Size to expand
      MEM_COMMIT,    // Both reserve and use the memory
      PAGE_READWRITE // Read-write permission
    );

    assert(ptr != MAP_FAILED);
    if (ptr == MAP_FAILED) return NULL;
    return mark_sentinel(ptr, size);
  }
#else 
  static inline void* expand_heap(size_t size) {
    void* ptr = mmap(
      NULL,         // Let the OS decide the address
      size,         // Size to expand
      PROT_READ | PROT_WRITE, // Read-write permission
      MAP_PRIVATE | MAP_ANONYMOUS, // No shared memory when forked | Memory not pegged to a file
      -1, 0         // Therfore no file descriptor and offest needed 
    );

    assert(ptr != MAP_FAILED);
    if (ptr == MAP_FAILED) return NULL;
    return mark_sentinel(ptr, size);
  }
#endif

static inline void* prepare_remaining_chunk(size_t* ptr, size_t size) {
  size_t header = size << RESERVED_BITS; // prev_free off by default
  header |= BEEN_FREED_MASK; // on
  ptr[0] = header;
  memcpy((char*)ptr + size - sizeof(size_t), &size, sizeof(size_t)); // footer at the end
  assert((size_t)(ptr + 1) % PAYLOAD_ALIGN == 0); // Paylad should be 16 byte aligned
  return ptr + 1;
}

static inline void* prepare_primary_chunk(size_t* ptr, size_t size) {
  size_t header = size << RESERVED_BITS; // It's not free, and prev_free is set at allocation
  ptr[0] = header;
  assert((size_t)(ptr + 1) % PAYLOAD_ALIGN == 0);
  return ptr + 1;
}

static inline bool chunk_is_free(size_t* chunk) {
  size_t header = *(chunk - 1);
  return header & BEEN_FREED_MASK ? true : false;
}

#ifdef _WIN32
  int shrink_heap(void* ptr, size_t size) {
    int result = VirtualFree(ptr, size, MEM_DECOMMIT);
    assert(result != 0); // Returns 1 when passes; 0 when fails
    return result ? 0 : -1;
  } 
#else
  int shrink_heap(void* ptr, size_t size) {
    int result = munmap(ptr, size);
    assert(result != -1); // Returns 0 when passes; -1 when fails
    return result;
  }
#endif

void* ulalloc(size_t size) {
  if (!size || size > MAX_SIZE) return NULL; 

  // Calculate extra bytes for storing (size_t) header in the allocated memory. 
  size += sizeof(size_t); 
  size = size < MIN_SIZE ? MIN_SIZE : size; // To contain all the metadata when freed, a chunk at minimum needs to be MIN_SIZE
  size_t chunk_size = size;

  // Look for memory inside the seglist first. 
  size_t* header_ptr = get_free_chunk_from_seglist(size); // returns pointer to header if available else NULL

  // If not found in seglist, beg the OS for more memory
  if (header_ptr == NULL) {
    chunk_size += sizeof(size_t) * 2;    // extra sizeof(size_t) * 2 bytes for the sentinels
    chunk_size = chunk_size <= MIN_ALLOC ? MIN_ALLOC : ALIGN_UP(chunk_size, PAGE_SIZE); // Make sure the req_size is a multiple of 4KiB 
    header_ptr = expand_heap(chunk_size); // expadn_heap already offsets the header_ptr past the prologue (left sentinel)
    if (header_ptr == NULL) return NULL;  // Neither the seglist nor the os has enough memory to lend; return NULL
    assert((size_t)header_ptr % 8 == 0 && (size_t)header_ptr % PAYLOAD_ALIGN != 0); // header location should not be a multiple of PAYLOAD_ALIGN
    chunk_size -= sizeof(size_t) * 2;    // Remove the extra bytes taken up by the sentinels from the usuable chunk
  } else { // Extract the size from the header
    chunk_size = (*header_ptr) >> RESERVED_BITS;
  }

  // Take up the necessary padding spaces and keep the extra bytes if they are less than MIN_SIZE
  size_t req_size = ALIGN_UP(size, PAYLOAD_ALIGN);
  size_t remaining = chunk_size - req_size; // remaining bytes. Should never be < 0
  if (remaining >= MIN_SIZE && chunk_size <= MIN_ALLOC) {
    int index = get_bucket_index(remaining);
    size_t* remaining_header_ptr = (size_t*)((char*)header_ptr + req_size);
    void* remaining_chunk = prepare_remaining_chunk(remaining_header_ptr, remaining);
    assert((size_t)header_ptr % 8 == 0 && (size_t)header_ptr % PAYLOAD_ALIGN != 0); 
    list_insert(&seglist[index], remaining_chunk);
  } else {
    req_size += remaining;
    assert(req_size == chunk_size);
  }
  
  return prepare_primary_chunk(header_ptr, req_size);
}


void ulfree(void* ptr) {
  if (chunk_is_free(ptr)) {
    fprintf(stderr, "Fatal Error: Attempted double free!\n");
    exit(EXIT_FAILURE); // Exits cleanly with failure code
  }
  size_t* header_ptr = (size_t*)ptr - 1;
  size_t header = *header_ptr;
  size_t size = header >> RESERVED_BITS;
  size_t* next_header_ptr = (size_t*)((char*)header_ptr + size);
  size_t next_header = *next_header_ptr;

  if (size > MIN_ALLOC - sizeof(size_t) * 2) { // Subtract sizeof(size_t) * 2 for the two sentinels
    shrink_heap(
      header_ptr - 1,           // Pointer to prologue (left sentinel) 
      size + sizeof(size_t) * 2 // +sizeof(size_t) for both sentinels
    );
    return;
  }

  // Merge right neighbour if free
  if (next_header & BEEN_FREED_MASK) {
    size += next_header >> RESERVED_BITS;
    list_remove((FreeNode*)(next_header_ptr + 1));
  }

  // Merge left neighbour if free
  if (header & PREV_FREE_MASK) {
    size_t* prev_footer_ptr = header_ptr - 1;
    size_t prev_size = *prev_footer_ptr;
    header_ptr = (size_t*)((char*)header_ptr - prev_size);
    list_remove((FreeNode*)(header_ptr + 1));
    size += prev_size;
  }

  // Shrink heap if the whole arena has been freed
  if (size == MIN_ALLOC - sizeof(size_t) * 2) { // Subtract sizeof(size_t) * 2 for the two sentinels
    shrink_heap(header_ptr - 1, MIN_ALLOC);     // Pointer to prologue (left sentinel) 
    return;
  }

  // Prepare new header
  header = size << RESERVED_BITS; // prev_free off by default.
  header |= BEEN_FREED_MASK; // on
  
  // Set header and footer
  header_ptr[0] = header;
  memcpy((char*)header_ptr + size - sizeof(size_t), &size, sizeof(size_t));

  // Insert to seglist
  int index = get_bucket_index(size);
  FreeNode* node = (FreeNode*)(header_ptr + 1);
  list_insert(&seglist[index], node);

  // Mark prev_free for the next chunk
  size_t* new_next_header_ptr = (size_t*)((char*)header_ptr + size);                                                                               // size_t* new_next_header_ptr = (size_t*)((char*)header_ptr + size);
  *new_next_header_ptr |= PREV_FREE_MASK;
} 

