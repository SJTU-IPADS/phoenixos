#include <iostream>

#include <assert.h>
#include <dlfcn.h>
#include <string.h>

#include "cudam.h"

static void *(*dlopen_orig)(const char *, int) = NULL;
static int (*dlclose_orig)(void *) = NULL;
static void *dl_handle = NULL;

void *dlopen(const char *filename, int flag)
{
    void *ret = NULL;

    if (dlopen_orig == NULL) {
        if ((dlopen_orig = dlsym(RTLD_NEXT, "dlopen")) == NULL) {
            assert(0);
        }
    }

    if (filename == NULL) {
        assert(dlopen_orig != NULL);
        return dlopen_orig(filename, flag);
    }

    

    /*!
     *  \note   redirect the open of libcuda.so / libnvidia-ml.so to cudam
     *          see https://github.com/pytorch/pytorch/blob/main/c10/cuda/driver_api.cpp#L12
     */
    if (filename != NULL 
        && (
            // strcmp(filename, "libcuda.so.1") == 0
            // || strcmp(filename, "libcuda.so") == 0
            strcmp(filename, "libnvidia-ml.so.1") == 0
            || strcmp(filename, "libnvidia-ml.so") == 0
        )
    ){
        dl_handle = dlopen_orig("libcudam.so", flag);
        return dl_handle;
    } else {
        return dlopen_orig(filename, flag);
    }
}


int dlclose(void *handle)
{
    if (handle == NULL) {
        assert(0);
    } else if (dlclose_orig == NULL) {
        if ((dlclose_orig = dlsym(RTLD_NEXT, "dlclose")) == NULL) {
            assert(0);
        }
    }

    // Ignore dlclose call that would close this library
    if (dl_handle == handle) {
        return 0;
    } else {
        return dlclose_orig(handle);
    }
}