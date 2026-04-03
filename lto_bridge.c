/*
 * LTO Symbol Force Module for ESP8266
 *
 * This file is compiled as a NORMAL project source file — WITH -flto.
 * It references every symbol that the pre-compiled non-LTO newlib archives
 * (libc.a, libstdc++.a, libgcc.a) need from the LTO-compiled Arduino core.
 *
 * Because these references exist in LTO bytecode, the GCC LTO plugin is
 * forced to "materialise" (emit machine code for) the corresponding
 * definitions from other LTO objects (heap.cpp, libc_replacements.cpp, etc.).
 * The materialised symbols then satisfy the non-LTO archive references
 * during the regular linker resolution pass.
 *
 * NOTE: This file must NOT be compiled with -fno-lto.  It must participate
 * in LTO so the plugin sees the references from within the LTO world.
 */

#include <stddef.h>  /* size_t */

/* Forward declarations — we only need the symbol references, not headers */
/* These are all extern "C" in the Arduino core */

/* ---------- heap.cpp / umm_malloc ---------- */
extern void *malloc(size_t);
extern void  free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);

struct _reent;
struct stat;
extern void *_malloc_r(struct _reent *, size_t);
extern void  _free_r(struct _reent *, void *);
extern void *_calloc_r(struct _reent *, size_t, size_t);
extern void *_realloc_r(struct _reent *, void *, size_t);

/* ---------- libc_replacements.cpp ---------- */
extern int _read_r(struct _reent *, int, char *, int);
extern int _write_r(struct _reent *, int, char *, int);
extern int _lseek_r(struct _reent *, int, int, int);
extern int _close_r(struct _reent *, int);
extern int _fstat_r(struct _reent *, int, struct stat *);

/* ---------- core_esp8266_postmortem.cpp ---------- */
extern void abort(void);
extern void _exit(int);

/*
 * Force the LTO plugin to keep every symbol above.
 * __attribute__((used))              — prevent the compiler from removing it
 * __attribute__((externally_visible)) — prevent LTO from internalising it
 * volatile                           — prevent optimisation of the stores
 */
__attribute__((used, externally_visible))
void _lto_force_symbols(void)
{
    volatile void *sink;

    /* heap */
    sink = (void *)(size_t)malloc;
    sink = (void *)(size_t)free;
    sink = (void *)(size_t)calloc;
    sink = (void *)(size_t)realloc;

    /* reentrant heap */
    sink = (void *)(size_t)_malloc_r;
    sink = (void *)(size_t)_free_r;
    sink = (void *)(size_t)_calloc_r;
    sink = (void *)(size_t)_realloc_r;

    /* syscall stubs */
    sink = (void *)(size_t)_read_r;
    sink = (void *)(size_t)_write_r;
    sink = (void *)(size_t)_lseek_r;
    sink = (void *)(size_t)_close_r;
    sink = (void *)(size_t)_fstat_r;

    /* C runtime */
    sink = (void *)(size_t)abort;
    sink = (void *)(size_t)_exit;

    (void)sink;
}
