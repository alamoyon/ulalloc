#define ULALLOC_IMPLEMENTATION
#define ULALLOC_TEST

#include "ulalloc.h"
#include <stdio.h>
#include <assert.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>


// Data survives ulrealloc. Write a pattern, realloc, and verify every byte.
// ✅️ Coalescing in isolation. Right-only merge, left-only merge, and reverse-order frees 
// ✅️ Bucket placement. ul_get_bucket_index has boundary cases (32, 33, 64, 65, and everything at or above 32KiB clamping to the last bucket)
// Realloc paths. Shrinking (which should split off a remainder), 
// Growing by merging with the left neighbor, growing by merging withj both neighbors, and realloc to the same size.
// Edge inputs:
  // ✅️ ulalloc(0) and ulalloc(SIZE_MAX) should return NULL.
  // ulfree(NULL) and ulrealloc(NULL, n) look like they'd crash, because chunk_is_free and ulrealloc dereference ptr - 1 unconditionally. Decide whether you want malloc semantics and test for it.
  // ulrealloc(ptr, 0) returns NULL without freeing ptr, which leaks. Standard realloc behavior differs, so decide what you want here too.
// Alignment. Every returned pointer should satisfy (uintptr_t)ptr % PAYLOAD_ALIGN == 0. Cheap to add and worth checking in every test.
// Data survives ulrealloc. Write a pattern, realloc, and verify every byte. This is the one that would catch the two bugs I mentioned earlier, and it's the most important missing test.
// ✅️ Coalescing in isolation. Right-only merge, left-only merge, and reverse-order frees (chunk3, chunk2, chunk1) each hit different branches in ulfree. Your current test only exercises both merges at once.
// ✅️ Reuse from the ulseglist. Alloc, free, then alloc the same size and check you get the same block back. This exercises get_free_from_seglist, which currently only runs when the fresh arena's leftover gets consumed.
// Realloc paths. Shrinking (which should split off a remainder), growing by merging with the left neighbor, growing by merging with both neighbors, and realloc to the same size.
// Double free. It calls exit, so testing it means fork()-ing and checking the child's exit status.




//////////////////
/// Test Utils ///
//////////////////
#define STR_LEN 128 
static char exp_str[STR_LEN];
static char ret_str[STR_LEN];

static bool assert_has_failed = false;

static void* g_bytes_8  = NULL;
static void* g_bytes_40 = NULL;

// Arguments 
#define NONE
#define BYTES_8 g_bytes_8
#define BYTES_40 g_bytes_40
#define BYTES_8_40 g_bytes_8, g_bytes_40

// Get time 
static double time_ms;
static double get_time_ms() {
  struct timespec ts;
  timespec_get(&ts, TIME_UTC);
  return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

// Create fail log for printing
#define MAKE_FAIL_LOG(comp) " ╰─ Expected \033[32m(%s)\033[0m %s recieved \033[33m(%s)\033[0m\n", exp_str, comp, ret_str

// Convert expected and returned value to strings
static int str_bool  (char *d, bool x)         { return snprintf(d, STR_LEN, "%s",  x ? "true" : "false"); }
static int str_int   (char *d, int x)          { return snprintf(d, STR_LEN, "%d",  x); }
static int str_uint  (char *d, unsigned x)     { return snprintf(d, STR_LEN, "%u",  x); }
static int str_long  (char *d, long x)         { return snprintf(d, STR_LEN, "%ld", x); }
static int str_ulong (char *d, unsigned long x){ return snprintf(d, STR_LEN, "%lu", x); }
static int str_double(char *d, double x)       { return snprintf(d, STR_LEN, "%f",  x); }
static int str_cstr  (char *d, const char *x)  { return snprintf(d, STR_LEN, "%s",  x ? x : "(null)"); }
static int str_ptr   (char *d, const void *x)  { return snprintf(d, STR_LEN, "%p",  x); }
#define TO_STR(x, var_name) _Generic((x),                                   \
  bool:          str_bool,                                                  \
  int:           str_int,                                                   \
  unsigned:      str_uint,                                                  \
  long:          str_long,                                                  \
  unsigned long: str_ulong,                                                 \
  double:        str_double,                                                \
  float:         str_double,                                                \
  char*:         str_cstr,                                                  \
  const char*:   str_cstr,                                                  \
  default:       str_ptr                                                    \
) (var_name, (x))


// Print fail log
#define PRINT_FAIL_LOG(log, op) do {                                        \
  printf(" ╭ %s\n", log);                                                   \
  if (strcmp(op, ">") == 0) {                                               \
    printf(MAKE_FAIL_LOG("is not greater than"));                           \
  } else if (strcmp(op, "<") == 0) {                                        \
    printf(MAKE_FAIL_LOG("is not less than"));                              \
  } else if (strcmp(op, "<=") == 0) {                                       \
    printf(MAKE_FAIL_LOG("is not less than or equal to"));                  \
  } else if (strcmp(op, ">=") == 0) {                                       \
    printf(MAKE_FAIL_LOG("is not greater than or equal to"));               \
  } else if (strcmp(op, "==") == 0) {                                       \
    printf(MAKE_FAIL_LOG("is not equal to"));                               \
  } else if (strcmp(op, "!=") == 0) {                                       \
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
    PRINT_FAIL_LOG(log, #op);                                               \
  }                                                                         \
} while (0)                                                                 \

// Initizalize arguments
#define INIT_ARGS(opt) do {                                                 \
  if (strcmp(opt, "BYTES_8") == 0) {                                        \
    g_bytes_8 = ulalloc(8);                                                 \
  } else if (strcmp(opt, "BYTES_40") == 0) {                                \
    g_bytes_40 = ulalloc(40);                                               \
  } else if (strcmp(opt, "BYTES_8_40") == 0) {                              \
    g_bytes_8 = ulalloc(8);                                                 \
    g_bytes_40 = ulalloc(40);                                               \
  }                                                                         \
} while (0)                                                                 \

// Free arguments
#define FREE_ARGS() do {                                                    \
  if (g_bytes_8) {                                                          \
    ulfree(g_bytes_8);                                                      \
    g_bytes_8 = NULL;                                                       \
  }                                                                         \
  if (g_bytes_40) {                                                         \
    ulfree(g_bytes_40);                                                     \
    g_bytes_40 = NULL;                                                      \
  }                                                                         \
} while (0)                                                                 \

// Reset test globals
#define RESET_TEST_GLOBALS() do {                                           \
  ul_g_test_crashed = false;                                                \
  ul_g_test_noop = false;                                                   \
} while (0)                                                                 \

// Test the passsed functions
#define TEST(func, arg) do {                                                \
  printf("%s ", #func);                                                     \
  INIT_ARGS(#arg);                                                          \
  func(arg);                                                                \
  FREE_ARGS();                                                              \
  if (!assert_has_failed) printf("\033[32m(Passed)\033[0m\n");              \
  assert_has_failed = false;                                                \
  RESET_TEST_GLOBALS()                                                      \
} while (0)                                                                 \

//////////////////////
/// Internal Utils ///
//////////////////////
#define MAX_FREE_NODES 16                         // Max free nodes a test can pull out of one bucket
#define SENTINEL_SIZE (sizeof(size_t) * 2)        // Prologue + epilogue
#define ARENA_SIZE (UL_MIN_ALLOC - SENTINEL_SIZE) // Usable bytes inside a fresh UL_MIN_ALLOC arena
#define LAST_BUCKET (UL_NUM_BUCKETS - 1)          // Where the leftover of a fresh UL_MIN_ALLOC arena lands
#define BYTES_8_EXP_SIZE get_expected_chunk_size(8)
#define BYTES_40_EXP_SIZE get_expected_chunk_size(40)

typedef struct {
  size_t size;
  bool been_freed;
  bool prev_free;
} Header;

static inline Header extract_header(void* ptr) {
  size_t header = *((size_t*)ptr - 1);
  return (Header) {
    header >> UL_RESERVED_BITS,
    header & UL_BEEN_FREED_MASK,
    header & UL_PREV_FREE_MASK
  };
}

static inline size_t get_expected_chunk_size(size_t size) {
  size += sizeof(size_t);
  size = size < UL_MIN_SIZE ? UL_MIN_SIZE : size;
  size = UL_ALIGN_UP(size, UL_PAYLOAD_ALIGN);
  if (size < UL_MIN_ALLOC) return size;
  return UL_ALIGN_UP(size, UL_PAGE_SIZE) - sizeof(size_t) * 2;
}

static inline int get_total_free_chunk_num(void) {
  int total = 0;

  for (int i = 0; i < UL_NUM_BUCKETS; ++i) {
    ULFreeNode* sentinel = &ulseglist[i];

    for (ULFreeNode* curr = sentinel->next; curr != sentinel; curr = curr->next) {
      total++;
    }
  }

  return total;
}

static inline int extract_free_payloads_from_seglist (int index, size_t** buff, int max) {
  ULFreeNode* sentinel = &ulseglist[index];
  int counter = 0;

  for (ULFreeNode* curr = sentinel->next; curr != sentinel && counter < max; curr = curr->next) {
    buff[counter] = (size_t*)curr;
    counter++;
  }

  return counter;
}

void safe_ulfree(void* ptr) {
  if (ptr == NULL) {
    return;
  } else if (ptr == g_bytes_8) {
    ulfree(g_bytes_8);
    g_bytes_8 = NULL;
  } else if (ptr == g_bytes_40) {
    ulfree(g_bytes_40);
    g_bytes_40 = NULL;
  } else {
    ulfree(ptr);
    ptr = NULL;
  }
}


//////////////////////
/// Test functions ///
//////////////////////

// Basic general tests 
void test_auto_parameter_insertion(char* bytes_8, size_t* bytes_40){
  Header bytes_8_header = extract_header(bytes_8);
  Header bytes_40_header = extract_header(bytes_40);
  ASSERT(BYTES_8_EXP_SIZE, ==, bytes_8_header.size, "Expected bytes_8 size don't match");
  ASSERT(BYTES_40_EXP_SIZE, ==, bytes_40_header.size, "Expected bytes_40 size don't match");
}
void test_safe_free(char* bytes_8) {
  ASSERT(1, ==, get_total_free_chunk_num(), "Found more chunks than 1");
  safe_ulfree(bytes_8);  
  ASSERT(0, ==, get_total_free_chunk_num(), "The ulseglist has not been emptied");
}
void test_sentinel_values(size_t* bytes_8) {
  size_t prologue = *(bytes_8 - 2);
  size_t epilogue = *((bytes_8 - 3) + (UL_MIN_ALLOC / sizeof(size_t)));
  ASSERT(0, ==, prologue, "Prologue is not 0");
  ASSERT(UL_PREV_FREE_MASK, ==, epilogue, "Epilogue is not equal to UL_PREV_FREE_MASK");
}
void test_get_bucket_index() {
  ASSERT(0, ==, ul_get_bucket_index(10), "Bucket index doesn't match");
  ASSERT(0, ==, ul_get_bucket_index(UL_MIN_SIZE), "Bucket index doesn't match");
  ASSERT(1, ==, ul_get_bucket_index(60), "Bucket index doesn't match");
  ASSERT(LAST_BUCKET, ==, ul_get_bucket_index(UL_MIN_ALLOC), "Bucket index doesn't match");
}

// Alloc tests
void test_chunk_header_data_after_alloc(char* bytes_8) {
  Header byte_8_header = extract_header(bytes_8);
  ASSERT(false, ==, byte_8_header.prev_free, "'prev_free' flag is on for first chunk");
  ASSERT(false, ==, byte_8_header.been_freed, "Used chunk is marked free");
}
void test_alloc_edge_cases() {
  ASSERT(NULL, ==, ulalloc(0), "Asking for 0 bytes doesn't return NULL");
  ASSERT(NULL, ==, ulalloc(UL_MAX_ALLOC + 1), "Asking for bytes greater than UL_MAX_ALLOC doesn't return NULL");
  ASSERT(0, ==, get_total_free_chunk_num(), "ulseglist is not empty");
}
void test_payload_align_on_alloc(char* bytes_8) {
  ASSERT(0, ==, (size_t)bytes_8 % UL_PAYLOAD_ALIGN, "Payload doesn't align");
}
void test_alloc_and_write(char* bytes_8) {
  char* str = "12345678";
  memcpy(bytes_8, str, 8);
  ASSERT(0, ==, memcmp(str, bytes_8, 8), "Write failed!");
}
void test_prev_free_flag_after_prev_chunk_has_been_allocd_again(char* bytes_8, size_t* bytes_40) {
  safe_ulfree(bytes_8);
  bytes_8 = ulalloc(8);
  Header bytes_40_header = extract_header(bytes_40);
  ASSERT(false, ==, bytes_40_header.prev_free, "'prev_free' flag has been reset after previous chunk has been allocated again");
  safe_ulfree(bytes_8);
}
void test_alloc_less_than_min_size(char* bytes_8) {
  Header bytes_8_header = extract_header(bytes_8);
  ASSERT(UL_MIN_SIZE, ==, bytes_8_header.size, "Minimum chunk size does not match UL_MIN_SIZE");
}
void test_alloc_greater_than_min_alloc() {
  size_t* large_chunk = ulalloc(UL_MIN_ALLOC + 1);
  ASSERT(0, ==, get_total_free_chunk_num(), "Allocating a chunk with size greater than UL_MIN_ALLOC left chunks in ulseglist");
  Header large_chunk_header = extract_header(large_chunk);
  size_t large_chunk_exp_size = get_expected_chunk_size(UL_MIN_ALLOC + 1);
  ASSERT(large_chunk_exp_size, ==, large_chunk_header.size, "Expected chunk size for chunk with size greater than UL_MIN_ALLOC don't match");
  safe_ulfree(large_chunk);
}
void test_remaining_free_chunk(char* bytes_8) {
  size_t* remaining_chunk;
  extract_free_payloads_from_seglist(LAST_BUCKET, &remaining_chunk, 1);
  Header remaining_chunk_header = extract_header(remaining_chunk);
  size_t remaining_chunk_exp_size = ARENA_SIZE - BYTES_8_EXP_SIZE;
  ASSERT(false, ==, remaining_chunk_header.prev_free, "'prev_free' chunk is set true for remaining chunk");
  ASSERT(true, ==, remaining_chunk_header.been_freed, "'been_freed' chunk is set false for remaining chunk");
  ASSERT(remaining_chunk_exp_size, ==, remaining_chunk_header.size, "Expected size doesn't match for remaining chunk");
}

// Free tests
void test_free_edge_cases(char* bytes_8, size_t* bytes_40) {
  char* saved_bytes_8_ptr = bytes_8;
  safe_ulfree(bytes_8);
  ulfree(saved_bytes_8_ptr); // double free
  ASSERT(true, ==, ul_g_test_crashed, "No runtime crashes");
}
void test_prev_free_flag_after_prev_chunk_has_been_freed(char* bytes_8, size_t* bytes_40) {
  safe_ulfree(bytes_8);
  Header bytes_40_header = extract_header(bytes_40);
  ASSERT(true, ==, bytes_40_header.prev_free, "'prev_free' not flagged even after previous chunk has been freed");
}
void test_left_merge_header_values(char* bytes_8, size_t* bytes_40) {
  void* righmost_chunk = ulalloc(sizeof(size_t)); // Blocks arena from being freed
  safe_ulfree(bytes_8);
  safe_ulfree(bytes_40);
  size_t* remaining_chunk;
  size_t remaining_chunk_exp_size = BYTES_8_EXP_SIZE + BYTES_40_EXP_SIZE;
  int bucket_index = ul_get_bucket_index(remaining_chunk_exp_size);
  extract_free_payloads_from_seglist(bucket_index, &remaining_chunk, 1);
  Header remaining_chunk_header = extract_header(remaining_chunk);
  ASSERT(false, ==, remaining_chunk_header.prev_free, "'prev_free' chunk is set true for remaining chunk");
  ASSERT(true, ==, remaining_chunk_header.been_freed, "'been_freed' chunk is set false for remaining chunk");
  ASSERT(remaining_chunk_exp_size, ==, remaining_chunk_header.size, "Expected size doesn't match for remaining chunk");
  safe_ulfree(righmost_chunk);
}
void test_right_merge_header_values(char* bytes_8, size_t* bytes_40) {
  safe_ulfree(bytes_40);
  size_t* remaining_chunk;
  extract_free_payloads_from_seglist(LAST_BUCKET, &remaining_chunk, 1);
  size_t remaining_chunk_exp_size = ARENA_SIZE - BYTES_8_EXP_SIZE;
  Header remaining_chunk_header = extract_header(remaining_chunk);
  ASSERT(false, ==, remaining_chunk_header.prev_free, "'prev_free' chunk is set true for remaining chunk");
  ASSERT(true, ==, remaining_chunk_header.been_freed, "'been_freed' chunk is set false for remaining chunk");
  ASSERT(remaining_chunk_exp_size, ==, remaining_chunk_header.size, "Expected size doesn't match for remaining chunk");
}
void test_left_and_right_merge_header_value(char* bytes_8, size_t* bytes_40) {
  // Layout: [ --freed-- | bytes_40 | --freed-- | righmost_chunk ]
  // Freeing bytes_40 should merge it's both neighbours
  void* temp_chunk = ulalloc(8); // size: UL_MIN_SIZE/BYTES_8_40
  void* righmost_chunk = ulalloc(8); // Blocks arena from being fereed
  safe_ulfree(bytes_8);
  safe_ulfree(temp_chunk);
  safe_ulfree(bytes_40);

  size_t* remaining_chunk;
  size_t remaining_chunk_exp_size = BYTES_8_EXP_SIZE + BYTES_40_EXP_SIZE + UL_MIN_SIZE;
  int bucket_index = ul_get_bucket_index(remaining_chunk_exp_size);
  extract_free_payloads_from_seglist(bucket_index, &remaining_chunk, 1);
  Header remaining_chunk_header = extract_header(remaining_chunk);
  ASSERT(false, ==, remaining_chunk_header.prev_free, "'prev_free' chunk is set true for remaining chunk");
  ASSERT(true, ==, remaining_chunk_header.been_freed, "'been_freed' chunk is set false for remaining chunk");
  ASSERT(remaining_chunk_exp_size, ==, remaining_chunk_header.size, "Expected size doesn't match for remaining chunk");
  safe_ulfree(righmost_chunk);
}
void test_reuse_from_seglist_after_free(char* bytes_8, size_t* bytes_40) {
  void* saved_bytes_8_ptr = bytes_8;
  safe_ulfree(bytes_8);
  bytes_8 = ulalloc(8);
  ASSERT(saved_bytes_8_ptr, ==, bytes_8, "Pointers don't match");
  safe_ulfree(bytes_8);
}

// Realloc tests
void test_realloc_edge_cases(void* bytes_8, void* bytes_40) {
  // Pass 0 as the new size
  ASSERT(NULL, ==, ulrealloc(bytes_8, 0), "Asking for 0 bytes doesn't return NULL");
  ASSERT(2, ==, get_total_free_chunk_num(), "ulrealloc didn't free bytes_8");

  // Pass size greater UL_MIN_ALLOC as the new size
  ASSERT(NULL, ==, ulrealloc(NULL, UL_MAX_ALLOC + 1), "ulrealloc doesn't return NULL when allocaing more than UL_MAX_ALLOC");

  // Pass NULL as the ptr
  bytes_8 = ulrealloc(NULL, 8);
  ASSERT(BYTES_8_EXP_SIZE, ==, extract_header(bytes_8).size, "Passing NULL did not allocate BYTES_8_EXP_SIZE chunk");
  // TODO: Add test for passing freed chunk

  safe_ulfree(bytes_8);
}
void test_realloc_data_persistence(char* bytes_8) {
  char* str = "12345678";
  memcpy(bytes_8, str, 8);
  ASSERT(0, ==, memcmp(str, bytes_8, 8), "Data doesn't match");
  char* realloced_chunk = ulrealloc(bytes_8, 40);
  ASSERT(0, ==, memcmp(str, realloced_chunk, 8), "Data doesn't match after realloc");
  safe_ulfree(realloced_chunk);
}
// TODO: What happens when bytes_8 is internally freed without g_bytes_8 being set to NULL
// Ansewer: I could add special macros for testing specifically
void test_realloc_merging_neigbours(char* bytes_8, size_t* bytes_40) {
  void* righmost_chunk = ulalloc(sizeof(size_t)); // Blocks to-be freed bytes_40 merging with remaining chunk
  safe_ulfree(bytes_40);

  size_t* remaining_chunk;
  extract_free_payloads_from_seglist(LAST_BUCKET, &remaining_chunk, 1);
  size_t prev_rem_chunk_size = extract_header(remaining_chunk).size;

  size_t realloced_chunk_size = BYTES_8_EXP_SIZE + BYTES_40_EXP_SIZE - sizeof(size_t);
  char* realloced_chunk = ulrealloc(bytes_8, realloced_chunk_size);
  size_t realloced_chunk_exp_size = BYTES_8_EXP_SIZE + BYTES_40_EXP_SIZE;

  extract_free_payloads_from_seglist(LAST_BUCKET, &remaining_chunk, 1);
  size_t new_rem_chunk_size = extract_header(remaining_chunk).size;

  ASSERT(realloced_chunk_exp_size, ==, extract_header(realloced_chunk).size, "Expected realloc size doesn't match the given");
  ASSERT(prev_rem_chunk_size, ==, new_rem_chunk_size, "Remaining chunk has been resized");

  safe_ulfree(righmost_chunk);
  safe_ulfree(realloced_chunk);
}

int main() {
  // Basic test calls
  TEST(test_auto_parameter_insertion, BYTES_8_40);
  TEST(test_safe_free, BYTES_8);
  TEST(test_sentinel_values, BYTES_8);
  TEST(test_get_bucket_index, NONE);

  // Alloc tests
  TEST(test_alloc_and_write, BYTES_8);
  TEST(test_alloc_edge_cases, NONE);
  TEST(test_payload_align_on_alloc, BYTES_8);
  TEST(test_chunk_header_data_after_alloc, BYTES_8);
  TEST(test_prev_free_flag_after_prev_chunk_has_been_allocd_again, BYTES_8_40);
  TEST(test_alloc_less_than_min_size, BYTES_8);
  TEST(test_alloc_greater_than_min_alloc, NONE);
  TEST(test_remaining_free_chunk, BYTES_8);

  // Free tests
  TEST(test_free_edge_cases, BYTES_8_40);
  TEST(test_prev_free_flag_after_prev_chunk_has_been_freed, BYTES_8_40);
  TEST(test_left_merge_header_values, BYTES_8_40);
  TEST(test_right_merge_header_values, BYTES_8_40);
  TEST(test_left_and_right_merge_header_value, BYTES_8_40);
  TEST(test_reuse_from_seglist_after_free, BYTES_8_40);

  // Realloc tests
  TEST(test_realloc_edge_cases, BYTES_8_40);
  TEST(test_realloc_data_persistence, BYTES_8);

  return 0;
}
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
///  size_t expected_remaining_size = ARENA_SIZE - get_expected_chunk_size(requested_size);
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
///  expected_remaining_size = ARENA_SIZE - get_expected_chunk_size(new_requested_size);
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
///  // The new size can't fit inside the UL_MIN_ALLOC arena anymore, so realloc
///  // frees the current chunk (which frees the whole arena) and maps a new one.
///  // The leftover of the new mapping is not pushed to the ulseglist;
///  // it stays inside the returned chunk as padding, so the ulseglist should be empty
///  new_requested_size += UL_MIN_ALLOC;
///  chunk = ulrealloc(chunk, new_requested_size);
///  ASSERT(0, ==, get_total_free_chunk_num());
///
///  ulfree(chunk);
///  ASSERT(0, ==, get_total_free_chunk_num());
///}
