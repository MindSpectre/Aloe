module;
#include "c_api.h"
export module m;
export namespace n {
    struct Packet {
        mbuf* m = nullptr;
        inline int headroom() const noexcept { return mbuf_headroom(m); }
    };
}
