#include "../src/impl/exec.h"

#ifdef _WIN32
#include <fcntl.h>
#include <stdlib.h>
#endif

int main(int argc, char *argv[]) {
#ifdef _WIN32
  // Default every fopen() to binary mode. The engine opens its binary data
  // files (kwg, klv2, wmp) with plain "r"/"w"; Windows text mode would stop
  // reads at the first 0x1A byte and corrupt writes by expanding newlines.
  // Text parsing is unaffected: the io layer strips carriage returns.
  _set_fmode(_O_BINARY);
#endif
  process_command_default(argc, argv);
}
