#pragma once
struct linger { int l_onoff; int l_linger; };
#define SOL_SOCKET 1
#define SO_LINGER 13
typedef unsigned int socklen_t;
inline int setsockopt(int, int, int, const void*, socklen_t) { return 0; }
#define MSG_PEEK 2
#define MSG_DONTWAIT 0x40
inline int recv(int, void*, size_t, int) { return 0; }   // host: peer always closed
