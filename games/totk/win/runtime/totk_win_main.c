/* totk-win: boots the game's recompiled modules on this machine.
 *
 *   totk-win [--cache DIR] [--start SYMBOL] [--stack BYTES] main.nxm [subsdk0.nxm sdk.nxm ...]
 *
 * The .nxm files come from games/totk/win/tools/nso_manifest.py run on the player's own ExeFS. The runtime reads its
 * settings from the same TOTK_* environment variables as the Mac port (TOTK_OPERATION_MODE=handheld, ...).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host_runtime.h"

#define DEFAULT_START "_ZN2nn4init5StartEmmPFvvES2_S2_"   /* nn::init::Start(handle, arg, ready, inits, finis) */

int main(int argc, char **argv) {
    const char *cache = NULL, *start = DEFAULT_START;
    uint64_t stack = 8ull << 20;
    const char *paths[LOADER_MAX_MODULES];
    unsigned n = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cache") && i + 1 < argc) cache = argv[++i];
        else if (!strcmp(argv[i], "--start") && i + 1 < argc) start = argv[++i];
        else if (!strcmp(argv[i], "--stack") && i + 1 < argc) stack = strtoull(argv[++i], NULL, 0);
        else if (argv[i][0] == '-') { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
        else if (n < LOADER_MAX_MODULES) paths[n++] = argv[i];
    }
    if (!n) {
        fprintf(stderr, "usage: %s [--cache DIR] [--start SYMBOL] [--stack BYTES] main.nxm [subsdk0.nxm sdk.nxm ...]\n", argv[0]);
        return 2;
    }
    setvbuf(stderr, NULL, _IOLBF, 0);
    if (host_runtime_load(paths, n, cache)) return 1;
    return host_runtime_boot(start, stack) ? 1 : 0;
}
