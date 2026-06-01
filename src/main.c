#include "leash/builder.h"
#include "leash/control.h"
#include "leash/run/backend.h"
#include "leash/run/options.h"

#include <stdio.h>
#include <string.h>

static void usage(FILE *stream) {
  fprintf(stream, "usage: leash <command> [args]\n"
                  "\n"
                  "commands:\n"
                  "  run                 run an instance directly\n"
                  "  start <instance>    start a configured instance\n"
                  "  stop <instance>     request graceful shutdown\n"
                  "  kill <instance>     force shutdown an instance\n"
                  "  attach <instance>   start and ssh into an instance\n"
                  "  logs <instance>     print and follow the instance log file\n"
                  "  console <instance>  attach to the raw serial console\n"
                  "  ip <instance>       print cached/discovered instance IPs\n"
                  "  ssh <instance>      start and ssh into an instance\n"
                  "  list, ls            list instances\n"
                  "  build <config>      build a leash instance from a yaml config\n"
                  "  builder <name>      show a builder config\n");
}

int main(int argc, char **argv) {
  builder_ensure_defaults();

  if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
    usage(argc < 2 ? stderr : stdout);
    return argc < 2 ? 1 : 0;
  }

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
