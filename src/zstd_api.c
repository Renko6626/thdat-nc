/* The only file that knows how zstd gets linked.  Windows: static libzstd.a
 * and real symbols.  Linux: dlopen("libzstd.so.1") at runtime, so the build
 * needs no zstd headers and the binary has no hard dependency until -x/-c
 * actually hits a compressed entry. */
#include "zstd_api.h"

#include <string.h>

#include "common.h"

#ifdef _WIN32

#include <zstd.h>

int
zstd_open(zstd_api_t* api)
{
    api->compress_bound = ZSTD_compressBound;
    api->compress = ZSTD_compress;
    api->decompress = ZSTD_decompress;
    api->is_error = ZSTD_isError;
    api->error_name = ZSTD_getErrorName;
    api->library = NULL;
    return 1;
}

void
zstd_close(zstd_api_t* api)
{
    (void)api;
}

#else

#include <dlfcn.h>

/* memcpy into the function-pointer slot because ISO C won't let you cast
 * dlsym's void* to a function pointer directly (and -Wpedantic will nag). */
static int
load_symbol(void* library, const char* name, void* function_pointer_slot)
{
    void* symbol = dlsym(library, name);
    memcpy(function_pointer_slot, &symbol, sizeof(symbol));
    return symbol != NULL;
}

int
zstd_open(zstd_api_t* api)
{
    memset(api, 0, sizeof(*api));
    api->library = dlopen("libzstd.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!api->library)
        return fail("cannot load libzstd.so.1: %s", dlerror());
    int complete = load_symbol(api->library, "ZSTD_compressBound", &api->compress_bound) &&
        load_symbol(api->library, "ZSTD_compress", &api->compress) &&
        load_symbol(api->library, "ZSTD_decompress", &api->decompress) &&
        load_symbol(api->library, "ZSTD_isError", &api->is_error) &&
        load_symbol(api->library, "ZSTD_getErrorName", &api->error_name);
    if (!complete) {
        dlclose(api->library);
        api->library = NULL;
        return fail("libzstd.so.1 lacks the expected symbols");
    }
    return 1;
}

void
zstd_close(zstd_api_t* api)
{
    if (api->library)
        dlclose(api->library);
    api->library = NULL;
}

#endif
