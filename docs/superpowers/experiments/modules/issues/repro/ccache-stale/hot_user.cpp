#include <cstdint>
import probe.hot;

extern "C" std::uint32_t use_free(std::uint32_t v) { return probe::hot::mix_free(v); }
extern "C" std::uint32_t use_inline(std::uint32_t v) { return probe::hot::mix_inline(v); }
extern "C" std::uint32_t use_member(std::uint32_t v) { return probe::hot::Mixer{}.mix_member(v); }
int main() { return 0; }
