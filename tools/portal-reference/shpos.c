// Print the first N stronghold positions (block coords and chunk coords) for a seed.
#include "finders.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: shpos <mc> <seed> <n>\n"); return 1; }
    int mc = str2mc(argv[1]);
    if (mc < 0) { fprintf(stderr, "bad mc\n"); return 1; }
    uint64_t seed = (uint64_t) strtoll(argv[2], NULL, 10);
    int n = atoi(argv[3]);
    Generator g;
    setupGenerator(&g, mc, 0);
    applySeed(&g, DIM_OVERWORLD, seed);
    StrongholdIter sh;
    initFirstStronghold(&sh, mc, seed);
    for (int i = 0; i < n; i++) {
        int idx = sh.index, ring = sh.ringnum;
        if (nextStronghold(&sh, &g) <= 0 && i + 1 < n) n = i + 1;
        printf("%d %d %d %d %d %d\n", idx, ring, sh.pos.x, sh.pos.z, sh.pos.x >> 4, sh.pos.z >> 4);
    }
    return 0;
}
