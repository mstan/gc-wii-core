/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef GCN_GX_HOST_H
#define GCN_GX_HOST_H
#include "cpu/cpu.h"
#ifdef _WIN32
#include <direct.h>
static inline int gx_host_mkdir(const char* path) { return _mkdir(path); }
#else
#include <sys/stat.h>
static inline int gx_host_mkdir(const char* path) { return mkdir(path, 0777); }
#endif
#if defined(__i386__) || defined(__x86_64__)
#include <x86intrin.h>
static inline u64 gx_host_ticks(void) { return __rdtsc(); }
static inline void gx_host_relax(void) { _mm_pause(); }
#else
#include <time.h>
static inline u64 gx_host_ticks(void) {
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    return (u64)now.tv_sec * 1000000000u + (u64)now.tv_nsec;
}
static inline void gx_host_relax(void) { }
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(GCN_GX_SCALAR_ONLY)
#define GCN_GX_X86_SIMD 1
#else
#define GCN_GX_X86_SIMD 0
#endif
#endif
