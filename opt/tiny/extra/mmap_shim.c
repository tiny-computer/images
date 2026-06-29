/*
 * Android mprotect shim for Hangover/Wine
 *
 * Android forbids mprotect() on file-backed MAP_PRIVATE pages after COW.
 *
 * Instead of intercepting mmap (which copies ALL sections → OOM),
 * we intercept mprotect.  When it fails with EACCES (COW pages),
 * we convert each page on-demand to anonymous memory.
 *
 * === WHAT THIS DOES ===
 *   mmap(PE_file, MAP_PRIVATE|RW) → PASS THROUGH (file-backed, shared page cache)
 *   mprotect(.text, RX) → if EACCES → per-page convert to anon → retry → OK
 *   mprotect(.data, RW) → already RW, no call needed → NO copy
 *   mprotect(.rdata, R) → if EACCES → per-page convert → OK
 *   mprotect(anon_pages, X) → anon → never EACCES → PASS THROUGH
 *
 * === RISKS / BOUNDARY CONDITIONS ===
 *   1. Assumes errno == EACCES.  Some Android kernels may return EPERM.
 *      If so, extend the condition.
 *   2. MAP_FIXED|ANON replacing file-backed page: works on Linux, but
 *      SELinux on Android may block it.  Run 'getenforce' first;
 *      if Enforcing, try 'setenforce 0' for testing.
 *   3. Wine may call mprotect via direct syscall() → LD_PRELOAD can't
 *      intercept those.  This code only handles libc mprotect() calls.
 *   4. Thread safety: page conversion (unmap→copy→remap) is not atomic.
 *      Wine holds virtual_mutex during PE loading, so this is safe.
 *
 * === USAGE ===
 *   aarch64-linux-android-gcc -shared -fPIC -o libmmap_shim.so android_mmap_shim.c -ldl
 *   LD_PRELOAD=/data/local/tmp/libmmap_shim.so wine your_app.exe
 *   # Debug:
 *   MmapShim_Debug=1 LD_PRELOAD=/data/local/tmp/libmmap_shim.so wine your_app.exe
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* ---- mprotect intercept ---- */

typedef int (*real_mprotect_t)(void *, size_t, int);
typedef void *(*real_mmap_t)(void *, size_t, int, int, int, off_t);

static real_mprotect_t real_mprotect;
static real_mmap_t     real_mmap;
static int g_debug;
static long g_pages_converted;
static long g_bytes_converted;

static void init_real_funcs(void)
{
    if (real_mprotect) return;
    real_mprotect = (real_mprotect_t)dlsym(RTLD_NEXT, "mprotect");
    real_mmap     = (real_mmap_t)    dlsym(RTLD_NEXT, "mmap");
    if (!real_mprotect || !real_mmap) {
        fprintf(stderr, "[MmapShim] FATAL: dlsym %s\n", dlerror());
        _exit(1);
    }
}

__attribute__((constructor))
static void shim_init(void)
{
    const char *s = getenv("MmapShim_Debug");
    g_debug = s ? atoi(s) : 0;

    /* Force early resolution so first mprotect call doesn't deadlock */
    init_real_funcs();

    if (g_debug)
        fprintf(stderr, "[MmapShim] loaded, will convert COW pages on mprotect(EACCES)\n");
}

__attribute__((destructor))
static void shim_fini(void)
{
    if (g_debug || g_pages_converted)
        fprintf(stderr, "[MmapShim] done: %ld pages (%ld KB) converted to anon\n",
                g_pages_converted, g_bytes_converted / 1024);
}

/*
 * Convert a single file-backed page to anonymous at the same address.
 * Returns 0 on success, -1 on failure.
 */
static int convert_page_to_anon(void *page_addr, long page_size, int target_prot)
{
    unsigned char buf[4096];

    if (page_size != 4096) {
        fprintf(stderr, "[MmapShim] ERROR: unexpected page_size %ld\n", page_size);
        return -1;
    }

    /* 1. Read original content (page is still mapped and readable) */
    memcpy(buf, page_addr, page_size);

    /*
     * 2. MAP_FIXED|ANON: unmaps old file-backed page, creates new zero page.
     *    This is an atomic kernel operation — no other thread sees a hole.
     *    But between here and memcpy() below, the page contains zeroes.
     */
    void *p = real_mmap(page_addr, page_size, PROT_READ | PROT_WRITE,
                        MAP_FIXED | MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) {
        if (g_debug)
            fprintf(stderr, "[MmapShim] MAP_FIXED|ANON at %p failed: %s\n",
                    page_addr, strerror(errno));
        return -1;
    }

    /* 3. Restore original data to new anonymous page */
    memcpy(p, buf, page_size);

    /* 4. Apply caller's desired protection */
    if (real_mprotect(p, page_size, target_prot) != 0) {
        if (g_debug)
            fprintf(stderr, "[MmapShim] final mprotect(%p, %ld, 0x%x) failed: %s\n",
                    page_addr, page_size, target_prot, strerror(errno));
        return -1;
    }

    return 0;
}

int mprotect(void *addr, size_t len, int prot)
{
    init_real_funcs();

    int ret = real_mprotect(addr, len, prot);
    if (ret == 0) return 0;
    /* Some Android kernels return EPERM, some EACCES for COW+mprotect */
    if (errno != EACCES && errno != EPERM)
        return ret;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) return ret;

    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)(page_size - 1);
    uintptr_t e = ((uintptr_t)addr + len + page_size - 1) & ~(uintptr_t)(page_size - 1);
    size_t npages = (e - a) / page_size;

    if (g_debug)
        fprintf(stderr, "[MmapShim] mprotect %s %p+%zx proto=0x%x → converting %zu page(s)\n",
                errno == EACCES ? "EACCES" : "EPERM", addr, len, prot, npages);

    for (size_t i = 0; i < npages; i++) {
        void *pg = (void *)(a + i * page_size);
        if (convert_page_to_anon(pg, page_size, prot) != 0)
            return -1;
        g_pages_converted++;
        g_bytes_converted += page_size;
    }

    return 0;
}
