#include "ulalloc.h"
#include <stdio.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
  if (exp op req) return;                                                   \
  if (!assert_has_failed) {                                                 \
    printf("\033[33m(Failed)\033[0m\n");                                    \
    assert_has_failed = true;                                               \
  }                                                                         \
  TO_STR(exp, exp_str);                                                     \
  TO_STR(req, req_str);                                                     \
  PRINT_COMP_TEXT(op);                                                      \


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

void t_basic_allocation_and_writing() {
  int chunk_size = 5;
  int* chunk = ulalloc(chunk_size);

  for (int i = 0; i < chunk_size; ++i) {
    chunk[i] = i * i;
  }

  for (int i = 0; i < chunk_size; ++i) {
    ASSERT(chunk[i], ==, i * i);
  }

  ulfree(chunk);
}

void t_minimum_chunk_size_header_data() {
  int* chunk = ulalloc(10);
  Header header = extract_header(chunk);

  ASSERT(header.size, ==, MIN_SIZE);
  ASSERT(header.been_freed, ==, false);

  ulfree(chunk);
}

void t_allocates_extra_bytes_for_the_header() {
  int* chunk = ulalloc(MIN_SIZE);
  Header header = extract_header(chunk);
  size_t expected_size = ALIGN_UP(MIN_SIZE + sizeof(size_t), PAYLOAD_ALIGN);

  ASSERT(header.size, ==, expected_size);

  ulfree(chunk);
}


int main() {
  CALL_TEST(t_basic_allocation_and_writing);
  CALL_TEST(t_minimum_chunk_size_header_data);
  CALL_TEST(t_allocates_extra_bytes_for_the_header);

  return 0;
}
