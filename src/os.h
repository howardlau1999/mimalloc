#pragma once

#include "mimalloc.h"
#include "mimalloc-internal.h"
#include "mimalloc-atomic.h"

#include <string.h>  // strerror

#ifdef _MSC_VER
#pragma warning(disable:4996)  // strerror
#endif

static void* mi_align_up_ptr(void* p, size_t alignment) {
  return (void*)_mi_align_up((uintptr_t)p, alignment);
}

static void* mi_align_down_ptr(void* p, size_t alignment) {
  return (void*)_mi_align_down((uintptr_t)p, alignment);
}

// page size (initialized properly in `os_init`)
static size_t os_page_size = 4096;

// minimal allocation granularity
static size_t os_alloc_granularity = 4096;

// if non-zero, use large page allocation
static size_t large_os_page_size = 0;

// is memory overcommit allowed? 
// set dynamically in _mi_os_init (and if true we use MAP_NORESERVE)
static bool os_overcommit = true;

void* mi_os_mem_alloc_impl(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large);
void* mi_os_mem_alloc_aligned_impl(size_t over_size, size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, mi_stats_t* stats);
bool mi_os_mem_free_impl(void* addr, size_t size, bool was_committed);
int mi_os_commitx_impl(void* start, size_t csize, bool commit, bool conservative, bool* is_zero);
bool mi_os_resetx_impl(void* start, size_t csize, bool reset);
int mi_os_protectx_impl(void* start, size_t csize, bool protect);
size_t mi_os_numa_node_countx(void);
size_t mi_os_numa_nodex(void);

#if !defined(MI_USE_SBRK) && !defined(__wasi__)
static bool use_large_os_page(size_t size, size_t alignment) {
  // if we have access, check the size and alignment requirements
  if (large_os_page_size == 0 || !mi_option_is_enabled(mi_option_large_os_pages)) return false;
  return ((size % large_os_page_size) == 0 && (alignment % large_os_page_size) == 0);
}
#endif

#if defined(__wasi__)
#define MI_USE_SBRK
#endif

/* -----------------------------------------------------------
  aligned hinting
-------------------------------------------------------------- */

// On 64-bit systems, we can do efficient aligned allocation by using
// the 2TiB to 30TiB area to allocate those.

#if (MI_INTPTR_SIZE >= 8)
static mi_decl_cache_align _Atomic(uintptr_t)aligned_base;

// Return a MI_SEGMENT_SIZE aligned address that is probably available.
// If this returns NULL, the OS will determine the address but on some OS's that may not be 
// properly aligned which can be more costly as it needs to be adjusted afterwards.
// For a size > 1GiB this always returns NULL in order to guarantee good ASLR randomization; 
// (otherwise an initial large allocation of say 2TiB has a 50% chance to include (known) addresses 
//  in the middle of the 2TiB - 6TiB address range (see issue #372))

#define MI_HINT_BASE ((uintptr_t)2 << 40)  // 2TiB start
#define MI_HINT_AREA ((uintptr_t)4 << 40)  // upto 6TiB   (since before win8 there is "only" 8TiB available to processes)
#define MI_HINT_MAX  ((uintptr_t)30 << 40) // wrap after 30TiB (area after 32TiB is used for huge OS pages)

static void* mi_os_get_aligned_hint(size_t try_alignment, size_t size)
{
  if (try_alignment <= 1 || try_alignment > MI_SEGMENT_SIZE) return NULL;
  size = _mi_align_up(size, MI_SEGMENT_SIZE);
  if (size > 1*MI_GiB) return NULL;  // guarantee the chance of fixed valid address is at most 1/(MI_HINT_AREA / 1<<30) = 1/4096.
  #if (MI_SECURE>0)
  size += MI_SEGMENT_SIZE;        // put in `MI_SEGMENT_SIZE` virtual gaps between hinted blocks; this splits VLA's but increases guarded areas.
  #endif

  uintptr_t hint = mi_atomic_add_acq_rel(&aligned_base, size);
  if (hint == 0 || hint > MI_HINT_MAX) {   // wrap or initialize
    uintptr_t init = MI_HINT_BASE;
    #if (MI_SECURE>0 || MI_DEBUG==0)       // security: randomize start of aligned allocations unless in debug mode
    uintptr_t r = _mi_heap_random_next(mi_get_default_heap());
    init = init + ((MI_SEGMENT_SIZE * ((r>>17) & 0xFFFFF)) % MI_HINT_AREA);  // (randomly 20 bits)*4MiB == 0 to 4TiB
    #endif
    uintptr_t expected = hint + size;
    mi_atomic_cas_strong_acq_rel(&aligned_base, &expected, init);
    hint = mi_atomic_add_acq_rel(&aligned_base, size); // this may still give 0 or > MI_HINT_MAX but that is ok, it is a hint after all
  }
  if (hint%try_alignment != 0) return NULL;
  return (void*)hint;
}
#else
static void* mi_os_get_aligned_hint(size_t try_alignment, size_t size) {
  MI_UNUSED(try_alignment); MI_UNUSED(size);
  return NULL;
}
#endif

#if defined(MI_USE_PTHREADS)
static pthread_mutex_t mi_heap_grow_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* -----------------------------------------------------------
  Free memory
-------------------------------------------------------------- */

static bool mi_os_mem_free(void* addr, size_t size, bool was_committed, mi_stats_t* stats)
{
  if (addr == NULL || size == 0) return true; // || _mi_os_is_huge_reserved(addr)
  bool err = mi_os_mem_free_impl(addr, size, was_committed);
  if (was_committed) { _mi_stat_decrease(&stats->committed, size); }
  _mi_stat_decrease(&stats->reserved, size);
  return !err;  
}

/* -----------------------------------------------------------
   Primitive allocation from the OS.
-------------------------------------------------------------- */

// Note: the `try_alignment` is just a hint and the returned pointer is not guaranteed to be aligned.
static void* mi_os_mem_alloc(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large, mi_stats_t* stats) {
  mi_assert_internal(size > 0 && (size % _mi_os_page_size()) == 0);
  if (size == 0) return NULL;
  if (!commit) allow_large = false;
  if (try_alignment == 0) try_alignment = 1; // avoid 0 to ensure there will be no divide by zero when aligning
  #if defined(MI_USE_SBRK)
    MI_UNUSED(allow_large);
    *is_large = false;
    void* p = mi_heap_grow(size, try_alignment);
  #else
    void* p = mi_os_mem_alloc_impl(size, try_alignment, commit, allow_large, is_large);
  #endif
  mi_stat_counter_increase(stats->mmap_calls, 1);
  if (p != NULL) {
    _mi_stat_increase(&stats->reserved, size);
    if (commit) { _mi_stat_increase(&stats->committed, size); }
  }
  return p;
}

// Primitive aligned allocation from the OS.
// This function guarantees the allocated memory is aligned.
static void* mi_os_mem_alloc_aligned(size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, mi_stats_t* stats) {
  mi_assert_internal(alignment >= _mi_os_page_size() && ((alignment & (alignment - 1)) == 0));
  mi_assert_internal(size > 0 && (size % _mi_os_page_size()) == 0);
  mi_assert_internal(is_large != NULL);
  if (!commit) allow_large = false;
  if (!(alignment >= _mi_os_page_size() && ((alignment & (alignment - 1)) == 0))) return NULL;
  size = _mi_align_up(size, _mi_os_page_size());

  // try first with a hint (this will be aligned directly on Win 10+ or BSD)
  void* p = mi_os_mem_alloc(size, alignment, commit, allow_large, is_large, stats);
  if (p == NULL) return NULL;
  
  // if not aligned, free it, overallocate, and unmap around it
  if (((uintptr_t)p % alignment != 0)) {
    mi_os_mem_free(p, size, commit, stats);
    _mi_warning_message("unable to allocate aligned OS memory directly, fall back to over-allocation (%zu bytes, address: %p, alignment: %zu, commit: %d)\n", size, p, alignment, commit);
    if (size >= (SIZE_MAX - alignment)) return NULL; // overflow
    const size_t over_size = size + alignment;
    p = mi_os_mem_alloc_aligned_impl(over_size, size, alignment, commit, allow_large, is_large, stats);
  }

  mi_assert_internal(p == NULL || (p != NULL && ((uintptr_t)p % alignment) == 0));
  return p;
}

#define MI_HUGE_OS_PAGE_SIZE  (MI_GiB)

void* mi_os_alloc_huge_os_pagesx_impl(void* addr, size_t size, int numa_node);