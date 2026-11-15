#include <stdint.h>

_Static_assert(sizeof(uint32_t) == 4, "M1 smoke requires a 32-bit uint32_t");

uint32_t camstream_m1_c11_probe(void)
{
    return UINT32_C(0xC11);
}
