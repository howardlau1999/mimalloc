/* ----------------------------------------------------------------------------
Copyright (c) 2018-2021, Microsoft Research, Daan Leijen
This is free software; you can redistribute it and/or modify it under the
terms of the MIT license. A copy of the license can be found in the file
"LICENSE" at the root of this distribution.
-----------------------------------------------------------------------------*/

#include "os.h"


/* -----------------------------------------------------------
  Initialization.
  On windows initializes support for aligned allocation and
  large OS pages (if MIMALLOC_LARGE_OS_PAGES is true).
----------------------------------------------------------- */
bool _mi_os_decommit(void* addr, size_t size, mi_stats_t* stats);
bool _mi_os_commit(void* addr, size_t size, bool* is_zero, mi_stats_t* tld_stats);

bool _mi_os_has_overcommit(void) {
  return os_overcommit;
}

// OS (small) page size
size_t _mi_os_page_size(void) {
  return os_page_size;
}

// if large OS pages are supported (2 or 4MiB), then return the size, otherwise return the small page size (4KiB)
size_t _mi_os_large_page_size(void) {
  return (large_os_page_size != 0 ? large_os_page_size : _mi_os_page_size());
}

// round to a good OS allocation size (bounded by max 12.5% waste)
size_t _mi_os_good_alloc_size(size_t size) {
  size_t align_size;
  if (size < 512*MI_KiB) align_size = _mi_os_page_size();
  else if (size < 2*MI_MiB) align_size = 64*MI_KiB;
  else if (size < 8*MI_MiB) align_size = 256*MI_KiB;
  else if (size < 32*MI_MiB) align_size = 1*MI_MiB;
  else align_size = 4*MI_MiB;
  if (mi_unlikely(size >= (SIZE_MAX - align_size))) return size; // possible overflow?
  return _mi_align_up(size, align_size);
}

/* -----------------------------------------------------------
  OS API: alloc, free, alloc_aligned
----------------------------------------------------------- */

void* _mi_os_alloc(size_t size, mi_stats_t* tld_stats) {
  MI_UNUSED(tld_stats);
  mi_stats_t* stats = &_mi_stats_main;
  if (size == 0) return NULL;
  size = _mi_os_good_alloc_size(size);
  bool is_large = false;
  return mi_os_mem_alloc(size, 0, true, false, &is_large, stats);
}

void  _mi_os_free_ex(void* p, size_t size, bool was_committed, mi_stats_t* tld_stats) {
  MI_UNUSED(tld_stats);
  mi_stats_t* stats = &_mi_stats_main;
  if (size == 0 || p == NULL) return;
  size = _mi_os_good_alloc_size(size);
  mi_os_mem_free(p, size, was_committed, stats);
}

void  _mi_os_free(void* p, size_t size, mi_stats_t* stats) {
  _mi_os_free_ex(p, size, true, stats);
}

void* _mi_os_alloc_aligned(size_t size, size_t alignment, bool commit, bool* large, mi_stats_t* tld_stats)
{
  MI_UNUSED(&mi_os_get_aligned_hint); // suppress unused warnings
  MI_UNUSED(tld_stats);
  if (size == 0) return NULL;
  size = _mi_os_good_alloc_size(size);
  alignment = _mi_align_up(alignment, _mi_os_page_size());
  bool allow_large = false;
  if (large != NULL) {
    allow_large = *large;
    *large = false;
  }
  return mi_os_mem_alloc_aligned(size, alignment, commit, allow_large, (large!=NULL?large:&allow_large), &_mi_stats_main /*tld->stats*/ );
}

/* -----------------------------------------------------------
  OS memory API: reset, commit, decommit, protect, unprotect.
----------------------------------------------------------- */

// OS page align within a given area, either conservative (pages inside the area only),
// or not (straddling pages outside the area is possible)
static void* mi_os_page_align_areax(bool conservative, void* addr, size_t size, size_t* newsize) {
  mi_assert(addr != NULL && size > 0);
  if (newsize != NULL) *newsize = 0;
  if (size == 0 || addr == NULL) return NULL;

  // page align conservatively within the range
  void* start = (conservative ? mi_align_up_ptr(addr, _mi_os_page_size())
    : mi_align_down_ptr(addr, _mi_os_page_size()));
  void* end = (conservative ? mi_align_down_ptr((uint8_t*)addr + size, _mi_os_page_size())
    : mi_align_up_ptr((uint8_t*)addr + size, _mi_os_page_size()));
  ptrdiff_t diff = (uint8_t*)end - (uint8_t*)start;
  if (diff <= 0) return NULL;

  mi_assert_internal((conservative && (size_t)diff <= size) || (!conservative && (size_t)diff >= size));
  if (newsize != NULL) *newsize = (size_t)diff;
  return start;
}

static void* mi_os_page_align_area_conservative(void* addr, size_t size, size_t* newsize) {
  return mi_os_page_align_areax(true, addr, size, newsize);
}

static void mi_mprotect_hint(int err) {
#if defined(MI_OS_USE_MMAP) && (MI_SECURE>=2) // guard page around every mimalloc page
  if (err == ENOMEM) {
    _mi_warning_message("the previous warning may have been caused by a low memory map limit.\n"
                        "  On Linux this is controlled by the vm.max_map_count. For example:\n"
                        "  > sudo sysctl -w vm.max_map_count=262144\n");
  }
#else
  MI_UNUSED(err);
#endif
}

// Commit/Decommit memory.
// Usually commit is aligned liberal, while decommit is aligned conservative.
// (but not for the reset version where we want commit to be conservative as well)
static bool mi_os_commitx(void* addr, size_t size, bool commit, bool conservative, bool* is_zero, mi_stats_t* stats) {
  // page align in the range, commit liberally, decommit conservative
  if (is_zero != NULL) { *is_zero = false; }
  size_t csize;
  void* start = mi_os_page_align_areax(conservative, addr, size, &csize);
  if (csize == 0) return true;  // || _mi_os_is_huge_reserved(addr))
  int err = 0;
  if (commit) {
    _mi_stat_increase(&stats->committed, size);  // use size for precise commit vs. decommit
    _mi_stat_counter_increase(&stats->commit_calls, 1);
  }
  else {
    _mi_stat_decrease(&stats->committed, size);
  }

  err = mi_os_commitx_impl(start, csize, commit, conservative, is_zero);
  
  if (err != 0) {
    _mi_warning_message("%s error: start: %p, csize: 0x%zx, err: %i\n", commit ? "commit" : "decommit", start, csize, err);
    mi_mprotect_hint(err);
  }
  mi_assert_internal(err == 0);
  return (err == 0);
}

bool _mi_os_commit(void* addr, size_t size, bool* is_zero, mi_stats_t* tld_stats) {
  MI_UNUSED(tld_stats);
  mi_stats_t* stats = &_mi_stats_main;
  return mi_os_commitx(addr, size, true, false /* liberal */, is_zero, stats);
}

bool _mi_os_decommit(void* addr, size_t size, mi_stats_t* tld_stats) {
  MI_UNUSED(tld_stats);
  mi_stats_t* stats = &_mi_stats_main;
  bool is_zero;
  return mi_os_commitx(addr, size, false, true /* conservative */, &is_zero, stats);
}

// Signal to the OS that the address range is no longer in use
// but may be used later again. This will release physical memory
// pages and reduce swapping while keeping the memory committed.
// We page align to a conservative area inside the range to reset.
static bool mi_os_resetx(void* addr, size_t size, bool reset, mi_stats_t* stats) {
  // page align conservatively within the range
  size_t csize;
  void* start = mi_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return true;  // || _mi_os_is_huge_reserved(addr)
  if (reset) _mi_stat_increase(&stats->reset, csize);
        else _mi_stat_decrease(&stats->reset, csize);
  if (!reset) return true; // nothing to do on unreset!

  #if (MI_DEBUG>1)
  if (MI_SECURE==0) {
    memset(start, 0, csize); // pretend it is eagerly reset
  }
  #endif

  return mi_os_resetx_impl(start, csize, reset);
}

// Signal to the OS that the address range is no longer in use
// but may be used later again. This will release physical memory
// pages and reduce swapping while keeping the memory committed.
// We page align to a conservative area inside the range to reset.
bool _mi_os_reset(void* addr, size_t size, mi_stats_t* tld_stats) {
  MI_UNUSED(tld_stats);
  mi_stats_t* stats = &_mi_stats_main;
  return mi_os_resetx(addr, size, true, stats);
}

// Protect a region in memory to be not accessible.
static  bool mi_os_protectx(void* addr, size_t size, bool protect) {
  // page align conservatively within the range
  size_t csize = 0;
  void* start = mi_os_page_align_area_conservative(addr, size, &csize);
  if (csize == 0) return false;
  /*
  if (_mi_os_is_huge_reserved(addr)) {
	  _mi_warning_message("cannot mprotect memory allocated in huge OS pages\n");
  }
  */
  int err = mi_os_protectx_impl(start, csize, protect);
  if (err != 0) {
    _mi_warning_message("mprotect error: start: %p, csize: 0x%zx, err: %i\n", start, csize, err);
    mi_mprotect_hint(err);
  }
  return (err == 0);
}

bool _mi_os_protect(void* addr, size_t size) {
  return mi_os_protectx(addr, size, true);
}

bool _mi_os_unprotect(void* addr, size_t size) {
  return mi_os_protectx(addr, size, false);
}

bool _mi_os_shrink(void* p, size_t oldsize, size_t newsize, mi_stats_t* stats) {
  // page align conservatively within the range
  mi_assert_internal(oldsize > newsize && p != NULL);
  if (oldsize < newsize || p == NULL) return false;
  if (oldsize == newsize) return true;

  // oldsize and newsize should be page aligned or we cannot shrink precisely
  void* addr = (uint8_t*)p + newsize;
  size_t size = 0;
  void* start = mi_os_page_align_area_conservative(addr, oldsize - newsize, &size);
  if (size == 0 || start != addr) return false;

#ifdef _WIN32
  // we cannot shrink on windows, but we can decommit
  return _mi_os_decommit(start, size, stats);
#else
  return mi_os_mem_free(start, size, true, stats);
#endif
}


/* ----------------------------------------------------------------------------
Support for allocating huge OS pages (1Gib) that are reserved up-front
and possibly associated with a specific NUMA node. (use `numa_node>=0`)
-----------------------------------------------------------------------------*/

#if !(MI_INTPTR_SIZE >= 8)
void* mi_os_alloc_huge_os_pagesx_impl(void* addr, size_t size, int numa_node) {
  MI_UNUSED(addr); MI_UNUSED(size); MI_UNUSED(numa_node);
  return NULL;
}
#endif

#if (MI_INTPTR_SIZE >= 8)
// To ensure proper alignment, use our own area for huge OS pages
static mi_decl_cache_align _Atomic(uintptr_t)  mi_huge_start; // = 0

// Claim an aligned address range for huge pages
static uint8_t* mi_os_claim_huge_pages(size_t pages, size_t* total_size) {
  if (total_size != NULL) *total_size = 0;
  const size_t size = pages * MI_HUGE_OS_PAGE_SIZE;

  uintptr_t start = 0;
  uintptr_t end = 0;
  uintptr_t huge_start = mi_atomic_load_relaxed(&mi_huge_start);
  do {
    start = huge_start;
    if (start == 0) {
      // Initialize the start address after the 32TiB area
      start = ((uintptr_t)32 << 40);  // 32TiB virtual start address
#if (MI_SECURE>0 || MI_DEBUG==0)      // security: randomize start of huge pages unless in debug mode
      uintptr_t r = _mi_heap_random_next(mi_get_default_heap());
      start = start + ((uintptr_t)MI_HUGE_OS_PAGE_SIZE * ((r>>17) & 0x0FFF));  // (randomly 12bits)*1GiB == between 0 to 4TiB
#endif
    }
    end = start + size;
    mi_assert_internal(end % MI_SEGMENT_SIZE == 0);
  } while (!mi_atomic_cas_strong_acq_rel(&mi_huge_start, &huge_start, end));

  if (total_size != NULL) *total_size = size;
  return (uint8_t*)start;
}
#else
static uint8_t* mi_os_claim_huge_pages(size_t pages, size_t* total_size) {
  MI_UNUSED(pages);
  if (total_size != NULL) *total_size = 0;
  return NULL;
}
#endif

// Allocate MI_SEGMENT_SIZE aligned huge pages
void* _mi_os_alloc_huge_os_pages(size_t pages, int numa_node, mi_msecs_t max_msecs, size_t* pages_reserved, size_t* psize) {
  if (psize != NULL) *psize = 0;
  if (pages_reserved != NULL) *pages_reserved = 0;
  size_t size = 0;
  uint8_t* start = mi_os_claim_huge_pages(pages, &size);
  if (start == NULL) return NULL; // or 32-bit systems

  // Allocate one page at the time but try to place them contiguously
  // We allocate one page at the time to be able to abort if it takes too long
  // or to at least allocate as many as available on the system.
  mi_msecs_t start_t = _mi_clock_start();
  size_t page;
  for (page = 0; page < pages; page++) {
    // allocate a page
    void* addr = start + (page * MI_HUGE_OS_PAGE_SIZE);
    void* p = mi_os_alloc_huge_os_pagesx_impl(addr, MI_HUGE_OS_PAGE_SIZE, numa_node);

    // Did we succeed at a contiguous address?
    if (p != addr) {
      // no success, issue a warning and break
      if (p != NULL) {
        _mi_warning_message("could not allocate contiguous huge page %zu at %p\n", page, addr);
        _mi_os_free(p, MI_HUGE_OS_PAGE_SIZE, &_mi_stats_main);
      }
      break;
    }

    // success, record it
    _mi_stat_increase(&_mi_stats_main.committed, MI_HUGE_OS_PAGE_SIZE);
    _mi_stat_increase(&_mi_stats_main.reserved, MI_HUGE_OS_PAGE_SIZE);

    // check for timeout
    if (max_msecs > 0) {
      mi_msecs_t elapsed = _mi_clock_end(start_t);
      if (page >= 1) {
        mi_msecs_t estimate = ((elapsed / (page+1)) * pages);
        if (estimate > 2*max_msecs) { // seems like we are going to timeout, break
          elapsed = max_msecs + 1;
        }
      }
      if (elapsed > max_msecs) {
        _mi_warning_message("huge page allocation timed out\n");
        break;
      }
    }
  }
  mi_assert_internal(page*MI_HUGE_OS_PAGE_SIZE <= size);
  if (pages_reserved != NULL) { *pages_reserved = page; }
  if (psize != NULL) { *psize = page * MI_HUGE_OS_PAGE_SIZE; }
  return (page == 0 ? NULL : start);
}

// free every huge page in a range individually (as we allocated per page)
// note: needed with VirtualAlloc but could potentially be done in one go on mmap'd systems.
void _mi_os_free_huge_pages(void* p, size_t size, mi_stats_t* stats) {
  if (p==NULL || size==0) return;
  uint8_t* base = (uint8_t*)p;
  while (size >= MI_HUGE_OS_PAGE_SIZE) {
    _mi_os_free(base, MI_HUGE_OS_PAGE_SIZE, stats);
    size -= MI_HUGE_OS_PAGE_SIZE;
    base += MI_HUGE_OS_PAGE_SIZE;
  }
}

/* ----------------------------------------------------------------------------
Support NUMA aware allocation
-----------------------------------------------------------------------------*/

_Atomic(size_t)  _mi_numa_node_count; // = 0   // cache the node count

size_t _mi_os_numa_node_count_get(void) {
  size_t count = mi_atomic_load_acquire(&_mi_numa_node_count);
  if (count <= 0) {
    long ncount = mi_option_get(mi_option_use_numa_nodes); // given explicitly?
    if (ncount > 0) {
      count = (size_t)ncount;
    }
    else {
      count = mi_os_numa_node_countx(); // or detect dynamically
      if (count == 0) count = 1;
    }    
    mi_atomic_store_release(&_mi_numa_node_count, count); // save it
    _mi_verbose_message("using %zd numa regions\n", count);
  }
  return count;
}

int _mi_os_numa_node_get(mi_os_tld_t* tld) {
  MI_UNUSED(tld);
  size_t numa_count = _mi_os_numa_node_count();
  if (numa_count<=1) return 0; // optimize on single numa node systems: always node 0
  // never more than the node count and >= 0
  size_t numa_node = mi_os_numa_nodex();
  if (numa_node >= numa_count) { numa_node = numa_node % numa_count; }
  return (int)numa_node;
}
