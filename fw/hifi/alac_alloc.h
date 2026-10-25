/* Forced into Apple's ALACDecoder.cpp (Makefile): its calloc/free, which <stdlib.h> does not
 * declare in a freestanding C++ build; defined in hifi/codec_alac.cpp on the runtime's heap */
#include <stddef.h>
extern "C" void *calloc(size_t a, size_t b);
extern "C" void free(void *p);
