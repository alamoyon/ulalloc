#include "ulalloc.h"
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


// Data survives ulrealloc. Write a pattern, realloc, and verify every byte.
// Coalescing in isolation. Right-only merge, left-only merge, and reverse-order frees 
// Bucket placement. get_bucket_index has boundary cases (32, 33, 64, 65, and everything at or above 32KiB clamping to the last bucket)
// Realloc paths. Shrinking (which should split off a remainder), 
// Growing by merging with the left neighbor, growing by merging withj both neighbors, and realloc to the same size.
// Edge inputs.
  // ulalloc(0) and ulalloc(SIZE_MAX) should return NULL.
  // ulfree(NULL) and ulrealloc(NULL, n) look like they'd crash, because chunk_is_free and ulrealloc dereference ptr - 1 unconditionally. Decide whether you want malloc semantics and test for it.
  // ulrealloc(ptr, 0) returns NULL without freeing ptr, which leaks. Standard realloc behavior differs, so decide what you want here too.
// Alignment. Every returned pointer should satisfy (uintptr_t)ptr % PAYLOAD_ALIGN == 0. Cheap to add and worth checking in every test.
// Data survives ulrealloc. Write a pattern, realloc, and verify every byte. This is the one that would catch the two bugs I mentioned earlier, and it's the most important missing test.
// Coalescing in isolation. Right-only merge, left-only merge, and reverse-order frees (chunk3, chunk2, chunk1) each hit different branches in ulfree. Your current test only exercises both merges at once.
// Reuse from the seglist. Alloc, free, then alloc the same size and check you get the same block back. This exercises get_free_from_seglist, which currently only runs when the fresh arena's leftover gets consumed.
// Bucket placement. get_bucket_index has boundary cases (32, 33, 64, 65, and everything at or above 32KiB clamping to the last bucket). A free chunk of each size should show up in the right bucket.
// Realloc paths. Shrinking (which should split off a remainder), growing by merging with the left neighbor, growing by merging with both neighbors, and realloc to the same size.
// Edge inputs.
// ulalloc(0) and ulalloc(SIZE_MAX) should return NULL.
// ulfree(NULL) and ulrealloc(NULL, n) look like they'd crash, because chunk_is_free and ulrealloc dereference ptr - 1 unconditionally. Decide whether you want malloc semantics and test for it.
// ulrealloc(ptr, 0) returns NULL without freeing ptr, which leaks. Standard realloc behavior differs, so decide what you want here too.
// Alignment. Every returned pointer should satisfy (uintptr_t)ptr % PAYLOAD_ALIGN == 0. Cheap to add and worth checking in every test.
// Double free. It calls exit, so testing it means fork()-ing and checking the child's exit status.
// Stress test. Many randomized alloc/free/realloc operations, filling each block with a per-block pattern and verifying it before freeing. Afterwards, assert that the seglist is empty. This finds interaction bugs that hand-written cases miss.
// Free list integrity. A helper that walks each bucket and checks node->next->prev == node, that every node's header is marked free, and that its footer matches its header. Call it at the end of the tests, or even after every operation in the stress test.




//////////////////
/// Test Utils ///
//////////////////
#define STR_LEN 64
#define NO_PARAMS void* _, void* __
static char exp_str[STR_LEN];
static char ret_str[STR_LEN];

static bool assert_has_failed = false;

// Arguments for CALL_TEST
enum ArgOptions { 
  NONE,
  BYTES_8,     // 8 bytes
  BYTES_40,    // MIN_SIZE + sizeof(size_t)
  BYTES_8_40,  // 8 and 40 bytes
};
static void* bytes_8  = NULL;
static void* bytes_40 = NULL;

static double time_ms;
static double get_time_ms() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// Create fail log for printing
#define MAKE_FAIL_LOG(comp) " ╰─ Expected \033[32m(%s)\033[0m %s recieved \033[33m(%s)\033[0m\n", exp_str, comp, ret_str

// Convert expected and returned value to strings
#define TO_STR(x, var_name) _Generic((x),                                   \
  bool: snprintf(var_name, STR_LEN, "%s", (bool)(x) ? "true" : "false"),    \
  int: snprintf(var_name, STR_LEN, "%d", (int)(x)),                         \
  unsigned long: snprintf(var_name, STR_LEN, "%lu", (unsigned long)(x)),    \
  default:       snprintf(var_name, STR_LEN, "%s",                          \
  (const char*)(uintptr_t)(x) ? (const char*)(uintptr_t)(x) : "(null)")     \
), var_name                                                                 \

// Print fail log
#define PRINT_FAIL_LOG(log, op) do {                                        \
  printf(" ╭ %s\n", log);                                                   \
  if (strcmp(#op, ">") == 0) {                                              \
    printf(MAKE_FAIL_LOG("is not greater than"));                           \
  } else if (strcmp(#op, "<") == 0) {                                       \
    printf(MAKE_FAIL_LOG("is not less than"));                              \
  } else if (strcmp(#op, "<=") == 0) {                                      \
    printf(MAKE_FAIL_LOG("is not less than or equal to"));                  \
  } else if (strcmp(#op, ">=") == 0) {                                      \
    printf(MAKE_FAIL_LOG("is not greater than or equal to"));               \
  } else if (strcmp(#op, "==") == 0) {                                      \
    printf(MAKE_FAIL_LOG("is not equal to"));                               \
  } else if (strcmp(#op, "!=") == 0) {                                      \
    printf(MAKE_FAIL_LOG("is equal to"));                                   \
  }                                                                         \
} while (0);                                                                \

// Convention: expected value, operation, returned
#define ASSERT(exp, op, ret, log) do {                                      \
  if (!((exp) op (ret))) {                                                  \
    if (!assert_has_failed) {                                               \
      printf("\033[33m(Failed)\033[0m\n");                                  \
      assert_has_failed = true;                                             \
    }                                                                       \
    TO_STR(exp, exp_str);                                                   \
    TO_STR(ret, ret_str);                                                   \
    PRINT_FAIL_LOG(log, op);                                                \
  }                                                                         \
} while (0)                                                                 \

// Initizalize arguments
#define INIT_ARGS(opt) do {                                                 \
  if (opt == BYTES_8) {                                                     \
    bytes_8 = ulalloc(8);                                                   \
  } else if (opt == BYTES_40) {                                             \
    bytes_40 = ulalloc(40);                                                 \
  } else if (opt == BYTES_8_40) {                                           \
    bytes_8 = ulalloc(8);                                                   \
    bytes_40 = ulalloc(40);                                                 \
  }                                                                         \
} while (0)                                                                 \

// Free arguments
#define FREE_ARGS() do {                                                    \
  if (bytes_8) {                                                            \
    ulfree(bytes_8);                                                        \
    bytes_8 = NULL;                                                         \
  }                                                                         \
  if (bytes_40) {                                                           \
    ulfree(bytes_40);                                                       \
    bytes_40 = NULL;                                                        \
  }                                                                         \
} while (0)                                                                 \

// Test the passsed functions
#define TEST(func, arg) do {                                                \
  printf("%s ", #func);                                                     \
  INIT_ARGS(arg);                                                           \
  func(bytes_8, bytes_40);                                                  \
  FREE_ARGS();                                                              \
  if (!assert_has_failed) printf("\033[32m(Passed)\033[0m\n");              \
  assert_has_failed = false;                                                \
} while (0)                                                                 \


//////////////////////
/// Internal Utils ///
//////////////////////
#define MAX_FREE_NODES 16                          // Max free nodes a test can pull out of one bucket
#define SENTINEL_SIZE  (sizeof(size_t) * 2)        // Prologue + epilogue
#define ARENA_PAYLOAD  (MIN_ALLOC - SENTINEL_SIZE) // Usable bytes inside a fresh MIN_ALLOC arena
#define LAST_BUCKET    (NUM_BUCKETS - 1)           // Where the leftover of a fresh MIN_ALLOC arena lands


typedef struct {
  size_t size;
  bool been_freed;
  bool prev_free;
} Header;

static inline Header extract_header(void* ptr) {
  size_t header = *((size_t*)ptr - 1);
  return (Header) {
    header >> RESERVED_BITS,
    header & BEEN_FREED_MASK,
    header & PREV_FREE_MASK
  };
}

static inline size_t get_expected_chunk_size(size_t size) {
  size += sizeof(size_t);
  size = size < MIN_SIZE ? MIN_SIZE : size;
  return ALIGN_UP(size, PAYLOAD_ALIGN);
}

static inline size_t get_expected_mapped_chunk_size(size_t size) {
  size_t chunk_size = get_expected_chunk_size(size) + SENTINEL_SIZE;
  return ALIGN_UP(chunk_size, PAGE_SIZE) - SENTINEL_SIZE;
}

static inline int get_total_free_chunk_num(void) {
  int total = 0;

  for (int i = 0; i < NUM_BUCKETS; ++i) {
    FreeNode* sentinel = &seglist[i];

    for (FreeNode* curr = sentinel->next; curr != sentinel; curr = curr->next) {
      total++;
    }
  }

  return total;
}

static inline int extract_free_payloads_from_seglist(int index, size_t** buff, int max) {
  FreeNode* sentinel = &seglist[index];
  int counter = 0;

  for (FreeNode* curr = sentinel->next; curr != sentinel && counter < max; curr = curr->next) {
    buff[counter] = (size_t*)curr;
    counter++;
  }

  return counter;
}

void test(NO_PARAMS) {
  ASSERT(true, ==, true, "True is indeed, true");
  ASSERT(false, ==, true, "OMG! False is not, true");
}

int main() {
  TEST(test, NONE);
  return 0;
}
/////////////////////////
////// Test functions ///
/////////////////////////
///static void t_basic_allocation_and_writing(void) {
///  int len = 5;
///  int* chunk = ulalloc(len * sizeof(int));
///
///  for (int i = 0; i < len; ++i) {
///    chunk[i] = i * i;
///  }
///
///  for (int i = 0; i < len; ++i) {
///    ASSERT(i * i, ==, chunk[i]);
///  }
///
///  ulfree(chunk);
///}
///
///static void t_minimum_chunk_size_header_data(void) {
///  size_t requested_size = 10;
///  size_t expected_size = MIN_SIZE;
///
///  int* chunk = ulalloc(requested_size);
///  Header header = extract_header(chunk);
///
///  ASSERT(expected_size, ==, header.size);
///  ASSERT(false, ==, header.been_freed);
///  ASSERT(false, ==, header.prev_free);
///
///  ulfree(chunk);
///}
///
///static void t_extra_bytes_for_the_header_and_padding(void) {
///  size_t requested_size = MIN_SIZE;
///  size_t expected_size = get_expected_chunk_size(requested_size);
///
///  int* chunk = ulalloc(requested_size);
///  Header header = extract_header(chunk);
///
///  ASSERT(expected_size, ==, header.size);
///
///  ulfree(chunk);
///}
///
///static void t_check_sentinel_values(void) {
///  size_t requested_size = 10 * sizeof(size_t);
///  size_t expected_prologue = 0;
///  size_t expected_epilogue = PREV_FREE_MASK;
///
///  size_t* chunk = ulalloc(requested_size);
///
///  // [prologue][header][payload ... ][epilogue]
///  // The payload starts two words in, and the epilogue is the very last word
///  size_t* prologue_ptr = chunk - 2;
///  size_t* epilogue_ptr = (size_t*)((char*)chunk + MIN_ALLOC - sizeof(size_t) * 3);
///
///  ASSERT(expected_prologue, ==, *prologue_ptr);
///  ASSERT(expected_epilogue, ==, *epilogue_ptr);
///
///  ulfree(chunk);
///}
///
///static void t_allocate_larger_than_min_alloc(void) {
///  size_t requested_size = MIN_ALLOC + 10;
///  size_t expected_size = get_expected_mapped_chunk_size(requested_size);
///
///  char* chunk = ulalloc(requested_size);
///  Header header = extract_header(chunk);
///
///  ASSERT(expected_size, ==, header.size);
///
///  ulfree(chunk);
///}
///
///static void t_allocate_multiple_chunks(void) {
///  size_t requested_size_1 = 10 * sizeof(size_t);
///  size_t requested_size_2 = 20;
///  size_t expected_size_1 = get_expected_chunk_size(requested_size_1);
///  size_t expected_size_2 = get_expected_chunk_size(requested_size_2);
///
///  size_t* chunk1 = ulalloc(requested_size_1);
///  char* chunk2 = ulalloc(requested_size_2);
///
///  Header header1 = extract_header(chunk1);
///  Header header2 = extract_header(chunk2);
///
///  ASSERT(false, ==, header1.been_freed);
///  ASSERT(false, ==, header2.been_freed);
///
///  ASSERT(false, ==, header1.prev_free);
///  ASSERT(false, ==, header2.prev_free);
///
///  ASSERT(expected_size_1, ==, header1.size);
///  ASSERT(expected_size_2, ==, header2.size);
///
///  ulfree(chunk1);
///  ulfree(chunk2);
///}
///
///static void t_remaining_free_chunk(void) {
///  size_t requested_size = sizeof(size_t*);
///  size_t* free_payloads[MAX_FREE_NODES] = { NULL };
///
///  size_t* chunk = ulalloc(requested_size);
///  ASSERT(1, ==, get_total_free_chunk_num());
///
///  int found = extract_free_payloads_from_seglist(LAST_BUCKET, free_payloads, MAX_FREE_NODES);
///  ASSERT(1, ==, found);
///  if (found != 1) { // Nothing to inspect; don't dereference an empty slot
///    ulfree(chunk);
///    return;
///  }
///
///  Header chunk_header = extract_header(chunk);
///  Header remaining_header = extract_header(free_payloads[0]);
///  size_t expected_remaining_size = ARENA_PAYLOAD - chunk_header.size;
///
///  ASSERT(true, ==, remaining_header.been_freed);
///  ASSERT(false, ==, remaining_header.prev_free);
///  ASSERT(expected_remaining_size, ==, remaining_header.size);
///
///  ulfree(chunk);
///}
///
///static void t_remaining_free_chunk_when_allocating_larger_than_min_alloc(void) {
///  size_t requested_size = MIN_ALLOC + 100;
///
///  char* chunk = ulalloc(requested_size);
///  ASSERT(0, ==, get_total_free_chunk_num());
///
///  ulfree(chunk);
///  ASSERT(0, ==, get_total_free_chunk_num());
///}
///
///static void t_freeing(void) {
///  size_t requested_size = 10 * sizeof(int);
///
///  ASSERT(0, ==, get_total_free_chunk_num());
///
///  int* chunk = ulalloc(requested_size);
///  ASSERT(1, ==, get_total_free_chunk_num());
///
///  ulfree(chunk);
///  ASSERT(0, ==, get_total_free_chunk_num());
///}
///
///static void t_ordered_freeing(void) {
///  size_t requested_size = 10 * sizeof(int);
///  size_t expected_size = get_expected_chunk_size(requested_size);
///
///  int* chunk1 = ulalloc(requested_size);
///  int* chunk2 = ulalloc(requested_size);
///
///  Header header = extract_header(chunk2);
///  ASSERT(false, ==, header.prev_free);
///  ASSERT(expected_size, ==, header.size);
///
///  // chunk1 can't merge with anything, so it's a separate free chunk next to the leftover
///  ulfree(chunk1);
///  header = extract_header(chunk2);
///  ASSERT(true, ==, header.prev_free);
///  ASSERT(expected_size, ==, header.size);
///  ASSERT(2, ==, get_total_free_chunk_num());
///
///  // chunk2 merges with both neighbours, which frees the whole arena
///  ulfree(chunk2);
///  ASSERT(0, ==, get_total_free_chunk_num());
///}
///
///static void t_realloc_chunk_header_data(void) {
///  size_t requested_size = 10 * sizeof(size_t);
///  size_t expected_size = get_expected_chunk_size(requested_size);
///
///  size_t* chunk = ulalloc(requested_size);
///  Header header = extract_header(chunk);
///  ASSERT(expected_size, ==, header.size);
///
///  size_t new_requested_size = requested_size + 5 * sizeof(size_t);
///  size_t new_expected_size = get_expected_chunk_size(new_requested_size);
///
///  chunk = ulrealloc(chunk, new_requested_size);
///  header = extract_header(chunk);
///  ASSERT(new_expected_size, ==, header.size);
///  ASSERT(false, ==, header.been_freed);
///  ASSERT(false, ==, header.prev_free);
///
///  ulfree(chunk);
///}
///
///static void t_remaining_chunk_size_on_realloc(void) {
///  size_t requested_size = 10 * sizeof(size_t);
///  size_t* free_payloads[MAX_FREE_NODES] = { NULL };
///
///  size_t* chunk = ulalloc(requested_size);
///
///  int found = extract_free_payloads_from_seglist(LAST_BUCKET, free_payloads, MAX_FREE_NODES);
///  ASSERT(1, ==, found);
///  if (found != 1) {
///    ulfree(chunk);
///    return;
///  }
///
///  Header remaining_header = extract_header(free_payloads[0]);
///  size_t expected_remaining_size = ARENA_PAYLOAD - get_expected_chunk_size(requested_size);
///  ASSERT(expected_remaining_size, ==, remaining_header.size);
///
///  // Growing in place should eat into the leftover chunk
///  size_t new_requested_size = requested_size + 5 * sizeof(size_t);
///  chunk = ulrealloc(chunk, new_requested_size);
///
///  found = extract_free_payloads_from_seglist(LAST_BUCKET, free_payloads, MAX_FREE_NODES);
///  ASSERT(1, ==, found);
///  if (found != 1) {
///    ulfree(chunk);
///    return;
///  }
///
///  remaining_header = extract_header(free_payloads[0]);
///  expected_remaining_size = ARENA_PAYLOAD - get_expected_chunk_size(new_requested_size);
///  ASSERT(expected_remaining_size, ==, remaining_header.size);
///
///  ulfree(chunk);
///}
///
///static void t_seglist_state_on_realloc(void) {
///  size_t requested_size = 10 * sizeof(size_t);
///  size_t* chunk = ulalloc(requested_size);
///
///  size_t new_requested_size = requested_size + 5 * sizeof(size_t);
///  chunk = ulrealloc(chunk, new_requested_size);
///  ASSERT(1, ==, get_total_free_chunk_num());
///
///  // The new size can't fit inside the MIN_ALLOC arena anymore, so realloc
///  // frees the current chunk (which frees the whole arena) and maps a new one.
///  // The leftover of the new mapping is not pushed to the seglist;
///  // it stays inside the returned chunk as padding, so the seglist should be empty
///  new_requested_size += MIN_ALLOC;
///  chunk = ulrealloc(chunk, new_requested_size);
///  ASSERT(0, ==, get_total_free_chunk_num());
///
///  ulfree(chunk);
///  ASSERT(0, ==, get_total_free_chunk_num());
///}
///
///int main(void) {
///  CALL_TEST(t_basic_allocation_and_writing);
///  CALL_TEST(t_minimum_chunk_size_header_data);
///  CALL_TEST(t_extra_bytes_for_the_header_and_padding);
///  CALL_TEST(t_check_sentinel_values);
///  CALL_TEST(t_allocate_larger_than_min_alloc);
///  CALL_TEST(t_allocate_multiple_chunks);
///  CALL_TEST(t_remaining_free_chunk);
///  CALL_TEST(t_remaining_free_chunk_when_allocating_larger_than_min_alloc);
///  CALL_TEST(t_freeing);
///  CALL_TEST(t_ordered_freeing);
///  CALL_TEST(t_realloc_chunk_header_data);
///  CALL_TEST(t_remaining_chunk_size_on_realloc);
///  CALL_TEST(t_seglist_state_on_realloc);
///  return 0;
///}
