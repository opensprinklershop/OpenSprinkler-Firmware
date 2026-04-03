/*
 * LTO Supplementary Linker Script for ESP8266 Arduino Core
 *
 * When Link-Time Optimization (-flto) is active, symbols defined in
 * LTO-compiled Arduino core objects (heap.cpp, libc_replacements.cpp, etc.)
 * are only available as LTO bytecode during the link phase.
 *
 * The pre-compiled non-LTO archives (libc.a, libstdc++.a from Newlib/GCC)
 * reference these symbols but can't resolve them until the LTO plugin
 * "materializes" them into real machine code.
 *
 * EXTERN() forces the linker to treat each symbol as externally referenced,
 * which instructs the LTO plugin to emit it before non-LTO archive
 * resolution takes place.
 *
 * Source mapping:
 *   heap.cpp             -> malloc, free, calloc, realloc,
 *                           _malloc_r, _free_r, _calloc_r, _realloc_r
 *   libc_replacements.cpp -> _read_r, _write_r, _lseek_r, _close_r, _fstat_r
 *   core_esp8266_postmortem.cpp -> abort
 */

/* ---- Heap allocators (heap.cpp / umm_malloc) ---- */
EXTERN(malloc)
EXTERN(free)
EXTERN(calloc)
EXTERN(realloc)

/* ---- Reentrant heap wrappers (heap.cpp) ---- */
EXTERN(_malloc_r)
EXTERN(_free_r)
EXTERN(_calloc_r)
EXTERN(_realloc_r)

/* ---- Newlib syscall stubs (libc_replacements.cpp) ---- */
EXTERN(_read_r)
EXTERN(_write_r)
EXTERN(_lseek_r)
EXTERN(_close_r)
EXTERN(_fstat_r)

/* ---- C++ runtime / abort (core_esp8266_postmortem.cpp) ---- */
EXTERN(abort)

/* ---- Additional symbols that may be needed ---- */
EXTERN(_exit)

/* ---- pvPortMalloc / pvPortFree (heap.cpp) ---- */
EXTERN(pvPortMalloc)
EXTERN(pvPortFree)
EXTERN(pvPortCalloc)
EXTERN(pvPortZalloc)
EXTERN(pvPortRealloc)
