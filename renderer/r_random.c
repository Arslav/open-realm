#include "r_random.h"

static uint32_t r_random = 1;

/* Presentation never consumes the simulation's libc RNG, even while their frames overlap. */
void R_SeedRandom(uint32_t seed) { r_random = seed ? seed : 1; }
float R_Random(void) {
    r_random ^= r_random << 13;
    r_random ^= r_random >> 17;
    r_random ^= r_random << 5;
    return (r_random >> 8) * (1.0f / 16777216.0f);
}
