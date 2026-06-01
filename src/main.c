#include "leash/builder.h"
#include "leash/control.h"
#include "leash/run/backend.h"
#include "leash/run/options.h"

#include <help_text.inc>
#include <stdio.h>
#include <string.h>

#ifndef LEASH_VERSION
#define LEASH_VERSION "0.0.0"
#endif

static void usage(FILE *stream) {
  fputs(help_text, stream);
}

int main(int argc, char **argv) {
  if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? 1 : 0;
  }

  if (!strcmp(argv[1], "--version")) {
    printf("leash %s\n", LEASH_VERSION);
    return 0;
  }

  builder_ensure_defaults();

  if (!strcmp(argv[1], "run")) {
    vm_run_options options;
    vm_run_options_init(&options);
    int parsed = vm_run_parse_options(&options, argc - 2, argv + 2);
    if (parsed != 0) {
      vm_run_options_free(&options);
      return parsed == 1 ? 0 : parsed;
    }
    int rc = vm_run_backend(&options);
    vm_run_options_free(&options);
    return rc;
  }

  if (!strcmp(argv[1], "build")) { return builder_build_main(argc - 2, argv + 2); }
  if (!strcmp(argv[1], "builder")) { return builder_info_main(argc - 2, argv + 2); }

  return leash_control_main(argc - 1, argv + 1);
}
