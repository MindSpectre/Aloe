#pragma once
struct mbuf { int len; };
static inline int mbuf_headroom(const mbuf* m) { return m->len; }
