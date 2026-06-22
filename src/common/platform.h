#pragma once

// Platform detection. The hot path backends are selected from these.

#if defined(__x86_64__) || defined(_M_X64)
#define CALIPER_X86_64 1
#else
#define CALIPER_X86_64 0
#endif

#if defined(__aarch64__) || defined(_M_ARM64)
#define CALIPER_ARM64 1
#else
#define CALIPER_ARM64 0
#endif

#if defined(__linux__)
#define CALIPER_LINUX 1
#else
#define CALIPER_LINUX 0
#endif

#if defined(__APPLE__)
#define CALIPER_APPLE 1
#else
#define CALIPER_APPLE 0
#endif

// The real hot path needs x86_64 Linux: rdtscp, AF_XDP, perf_event_open,
// isolcpus, PTP timestamps. Everything else builds the portable backend so
// the loop runs for development on any host.
#if CALIPER_X86_64 && CALIPER_LINUX
#define CALIPER_NATIVE 1
#else
#define CALIPER_NATIVE 0
#endif

#define CALIPER_CACHELINE 64

#if defined(__GNUC__) || defined(__clang__)
#define CALIPER_ALWAYS_INLINE inline __attribute__((always_inline))
#define CALIPER_LIKELY(x) __builtin_expect(!!(x), 1)
#define CALIPER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#define CALIPER_ALIGN(n) __attribute__((aligned(n)))
#else
#define CALIPER_ALWAYS_INLINE inline
#define CALIPER_LIKELY(x) (x)
#define CALIPER_UNLIKELY(x) (x)
#define CALIPER_ALIGN(n)
#endif
