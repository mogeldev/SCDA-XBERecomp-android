// Separate process: resolve the real runtime without starting SDL or calling game code.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static void* symbol(void* lib, const char* name) {
    dlerror();
    void* result = dlsym(lib, name);
    const char* error = dlerror();
    if (error) fprintf(stderr, "%s: %s\n", name, error);
    return error ? NULL : result;
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Load code first, as production does: its DT_NEEDED brings in the GLOBAL host.
    void* code = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!code) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    const uint32_t* lo = symbol(code, "X86CodeBase");
    const uint32_t* hi = symbol(code, "X86CodeEnd");
    const uint32_t* entry = symbol(code, "X86EntryPoint");
    const uint32_t* image = symbol(code, "X86ImageBase");
    const uint32_t* end = symbol(code, "X86ImageEnd");
    const uint32_t* thunk = symbol(code, "X86KernelThunk");
    const uint32_t* continuations = symbol(code, "X86Continuations");
    // Matches X86FuncMapping in src/cpu/x86_context.h on the supported 64-bit ABI.
    struct Mapping { uint32_t guest; void* host; };
    const struct Mapping* mappings = symbol(code, "X86FuncMappings");
    _Static_assert(sizeof(struct Mapping) == 16, "X86FuncMapping ABI mismatch");
    void* host_entry = symbol(code, "ScdaMain");
    if (!lo || !hi || !entry || !image || !end || !thunk || !continuations || !mappings || !host_entry) return 1;
    // Fixed validated USA XBE; update these checks if the probe gains other images.
    if (*lo != 0x11000 || *hi != 0x4F5150 || *entry != 0x54775 ||
            *image != 0x10000 || *end != 0x6D1540 || *thunk != 0x4F5160) {
        fprintf(stderr, "Unexpected XBE address metadata\n");
        return 1;
    }
    int found_entry = 0, overrides = 0;
    for (int i = 0; i < 33172; i++) {
        if (!mappings[i].host || mappings[i].guest < *lo || mappings[i].guest >= *hi ||
                (i && mappings[i].guest <= mappings[i - 1].guest)) {
            fprintf(stderr, "Invalid dispatch mapping %d\n", i);
            return 1;
        }
        char name[32];
        snprintf(name, sizeof(name), "sub_%08X", mappings[i].guest);
        // Relocations prefer GLOBAL host overrides over the code's weak definitions.
        void* target = dlsym(RTLD_DEFAULT, name);
        if (!target) target = symbol(code, name);
        if (!target || target != mappings[i].host) {
            fprintf(stderr, "Dispatch relocation mismatch: %s\n", name);
            return 1;
        }
        Dl_info info;
        if (!dladdr(target, &info)) return 1;
        const char* file = info.dli_fname;
        if (file) {
            const char* base = file;
            for (const char* p = file; *p; p++) if (*p == '/') base = p + 1;
            if (strcmp(base, "libscda_host.so") == 0) overrides++;
        }
        found_entry |= mappings[i].guest == *entry;
    }
    if (mappings[33172].host || mappings[33172].guest || !found_entry || !overrides) {
        fprintf(stderr, "Invalid dispatch terminator, missing entry or missing host overrides\n");
        return 1;
    }
    const uint32_t expected[] = {0x3EE32, 0x72FD4, 0x73286, 0x23123D, 0};
    for (int i = 0; i < 5; i++) {
        if (continuations[i] != expected[i]) {
            fprintf(stderr, "Invalid continuation %d\n", i);
            return 1;
        }
    }
    printf("RTLD_NOW: complete code and host dependencies resolved\n");
    printf("Dispatch: 33172 mappings, entry=%08X, 4 continuations; host overrides=%d\n", *entry, overrides);
    printf("ScdaMain resolved; no game functions called\nLOAD_CHECK_PASS\n");
    // Exit releases the runtime; avoid pretending that production supports unloading it.
    return 0;
}
