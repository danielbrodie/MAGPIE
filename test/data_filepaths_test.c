#include "data_filepaths_test.h"

#include "../src/ent/data_filepaths.h"
#include "../src/util/io_util.h"
#include "test_util.h"
#include <assert.h>
#include <stdlib.h>

static void test_crlf_file_read_is_byte_exact(void) {
  const char *filename = "data_filepaths_crlf_test.tmp";
  const char *contents = "first\r\nsecond\r\n";
  ErrorStack *error_stack = error_stack_create();

  write_string_to_file(filename, "wb", contents, error_stack);
  assert(error_stack_is_empty(error_stack));
  char *read_contents = get_string_from_file(filename, error_stack);
  assert(error_stack_is_empty(error_stack));
  assert_strings_equal(read_contents, contents);

  free(read_contents);
  delete_file(filename);
  error_stack_destroy(error_stack);
}

void test_data_filepaths(void) {
#ifdef _WIN32
  const char *data_paths = "C:\\first;D:\\second";
  const char *expected_filepath = "C:\\first/lexica/NWL23.txt";
#else
  const char *data_paths = "/first:/second";
  const char *expected_filepath = "/first/lexica/NWL23.txt";
#endif

  ErrorStack *error_stack = error_stack_create();
  char *filepath = data_filepaths_get_writable_filename(
      data_paths, "NWL23", DATA_FILEPATH_TYPE_LEXICON, error_stack);

  assert(filepath != NULL);
  assert_strings_equal(filepath, expected_filepath);

  free(filepath);
  error_stack_destroy(error_stack);
  test_crlf_file_read_is_byte_exact();
}
