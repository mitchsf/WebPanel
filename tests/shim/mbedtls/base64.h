#pragma once
#include <cstddef>
inline int mbedtls_base64_decode(unsigned char*, size_t, size_t* olen, const unsigned char*, size_t) { if (olen) *olen = 0; return -1; }
inline int mbedtls_base64_encode(unsigned char*, size_t, size_t* olen, const unsigned char*, size_t) { if (olen) *olen = 0; return -1; }
