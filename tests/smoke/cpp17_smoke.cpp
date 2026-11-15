#include <cstdint>
#include <type_traits>

extern "C" std::uint32_t camstream_m1_c11_probe(void);

static_assert(std::is_same_v<decltype(camstream_m1_c11_probe()), std::uint32_t>);

int main()
{
    return camstream_m1_c11_probe() == UINT32_C(0xC11) ? 0 : 1;
}
