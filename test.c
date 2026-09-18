#include "ulalloc.h"
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

//////////////////
/// Test Utils ///
//////////////////
#define STR_LEN 64
static bool assert_has_failed = false;

static char exp_str[STR_LEN];
static char req_str[STR_LEN];

#define CALL_TEST(func)                                                     \
  printf("%s ", #func);                                                     \
  func();                                                                   \
  if (!assert_has_failed) {                                                 \
    printf("\033[32m(Passed)\033[0m\n");                                    \
  }                                                                         \
  assert_has_failed = false;

#define MAKE_COMP_TEXT(msg) " - Expected \033[32m(%s)\033[0m %s recieved \033[33m(%s)\033[0m\n", exp_str, msg, req_str

// 1. I macro'd so hard, the neovim code highlighter is tweaking
// 2. Curse C for not allowing multi-line macros!
#define PRINT_COMP_TEXT(op)                                                 \
  if (strcmp(#op, ">") == 0) {                                              \
    printf(MAKE_COMP_TEXT("is not greater than"));                          \
  } else if (strcmp(#op, "<") == 0) {                                       \
    printf(MAKE_COMP_TEXT("is not less than"));                             \
  } else if (strcmp(#op, "<=") == 0) {                                      \
    printf(MAKE_COMP_TEXT("is not less than or equal to"));                 \
  } else if (strcmp(#op, ">=") == 0) {                                      \
    printf(MAKE_COMP_TEXT("is not greater than or equal to"));              \
  } else if (strcmp(#op, "==") == 0) {                                      \
    printf(MAKE_COMP_TEXT("is not equal to"));                              \
  } else if (strcmp(#op, "!=") == 0) {                                      \
    printf(MAKE_COMP_TEXT("is equal to"));                                  \
  }

#define TO_STR(x, var_name) _Generic((x),                                   \
  bool: snprintf(var_name, STR_LEN, "%s", (bool)(x) ? "true" : "false"),    \
  int: snprintf(var_name, STR_LEN, "%d", (int)(x)),                         \
  unsigned long: snprintf(var_name, STR_LEN, "%lu", (unsigned long)(x)),    \
  default:       snprintf(var_name, STR_LEN, "%s",                          \
  (const char*)(uintptr_t)(x) ? (const char*)(uintptr_t)(x) : "(null)")     \
), var_name                                                                 \

#define ASSERT(exp, op, req)                                                \
  do {                                                                      \
    if (!((exp) op (req))) {                                                \
      if (!assert_has_failed) {                                             \
        printf("\033[33m(Failed)\033[0m\n");                                \
        assert_has_failed = true;                                           \
      }                                                                     \
      TO_STR(exp, exp_str);                                                 \
      TO_STR(req, req_str);                                                 \
      PRINT_COMP_TEXT(op);                                                  \
    }                                                                       \
  } while (0)


//////////////////////
/// Internal Utils ///
//////////////////////
typedef struct {
  size_t size;
  bool been_freed;
  bool prev_free;
} Header;

Header extract_header(void* ptr) {
  size_t header = *((size_t*)ptr - 1);
  return (Header) {
    header >> RESERVED_BITS,
    header & BEEN_FREED_MASK,
    header & PREV_FREE_MASK
  };
}

static inline void inspect_seglist() {
  for (size_t i = 0; i < NUM_BUCKETS; ++i) {
    FreeNode* sentinel = &seglist[i];
    FreeNode* next_free_node = sentinel->next;
    if (sentinel != next_free_node) {
      printf("Bucket: %zu, %zu Bytes\n", i + 1, (size_t)32 << i);
    }

    for (FreeNode* curr = sentinel->next; curr != sentinel; curr = curr->next) {
      size_t header = *(size_t*)((char*)curr - sizeof(size_t));
      int is_free   = header &  BEEN_FREED_MASK ? 1 : 0;
      int prev_free = header &  PREV_FREE_MASK ? 1 : 0;
      size_t size   = header >> RESERVED_BITS;
      printf("(%zu) Is free: %d, Prev free: %d, Size: %zu\n", (size_t)curr, is_free, prev_free, size);
    }

    if (sentinel != next_free_node) {
      printf("\n");
    }
  }
  printf("--------------------------------------------------------\n");
}

//////////////////////
/// Test functions ///
//////////////////////
void t_basic_allocation_and_writing() {
  int chunk_size = 5;
  int* chunk = ulalloc(chunk_size * sizeof(int));

  for (int i = 0; i < chunk_size; ++i) {
    chunk[i] = i * i;
  }

  for (int i = 0; i < chunk_size; ++i) {
    ASSERT(i * i, ==, chunk[i]);
  }

  ulfree(chunk);
}

void t_minimum_chunk_size_header_data() {
  int* chunk = ulalloc(10);
  Header header = extract_header(chunk);

  ASSERT(MIN_SIZE, ==, header.size);
  ASSERT(false, ==, header.been_freed);

  ulfree(chunk);
}

void t_extra_bytes_for_the_header_and_padding() {
  int* chunk = ulalloc(MIN_SIZE);
  Header header = extract_header(chunk);
  size_t expected_size = ALIGN_UP(MIN_SIZE + sizeof(size_t), PAYLOAD_ALIGN);

  ASSERT(expected_size, ==, header.size);

  ulfree(chunk);
}

void t_check_sentinel_values() {
  size_t chunk_size = 10;
  size_t* chunk = ulalloc(chunk_size * sizeof(size_t));
  
  size_t prologue = *(chunk - 2);
  ASSERT(0, ==, prologue);

  size_t epilogue = *(size_t*)((char*)chunk + (MIN_ALLOC - sizeof(size_t) * 3));
  ASSERT(0 | PREV_FREE_MASK, ==, epilogue);
  
  ulfree(chunk);
}

void t_allocate_larger_than_min_alloc() {
  size_t chunk_size = MIN_ALLOC + 10;
  char* chunk = ulalloc(chunk_size);
  Header header = extract_header(chunk);

  size_t expected_chunk_size = ALIGN_UP(chunk_size + sizeof(size_t), PAYLOAD_ALIGN);
  expected_chunk_size = ALIGN_UP(expected_chunk_size, PAGE_SIZE) - sizeof(size_t) * 2;
  ASSERT(expected_chunk_size, ==, header.size);

  ulfree(chunk);
}

int main() {
  CALL_TEST(t_basic_allocation_and_writing);
  CALL_TEST(t_minimum_chunk_size_header_data);
  CALL_TEST(t_extra_bytes_for_the_header_and_padding);
  CALL_TEST(t_check_sentinel_values);
  CALL_TEST(t_allocate_larger_than_min_alloc);
  return 0;
}
