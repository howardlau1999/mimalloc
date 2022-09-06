#if defined(__wasi__)

#include "os.h"

#include <unistd.h>

static void* mi_memory_grow( size_t size ) {
  size_t base = (size > 0 ? __builtin_wasm_memory_grow(0,_mi_divide_up(size, _mi_os_page_size()))
                          : __builtin_wasm_memory_size(0));
  if (base == SIZE_MAX) return NULL;     
  return (void*)(base * _mi_os_page_size());    
}

void _mi_os_init(void) {
  os_overcommit = false;
  os_page_size = 64*MI_KiB; // WebAssembly has a fixed page size: 64KiB
  os_alloc_granularity = 16;
}

void* mi_os_mem_alloc_impl(size_t size, size_t try_alignment, bool commit, bool, bool*) {
  *is_large = false;
  return mi_heap_grow(size, try_alignment);
}

void* mi_os_mem_alloc_aligned_impl(size_t over_size, size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, mi_stats_t* stats) { 
  // overallocate...
  void* p = mi_os_mem_alloc(over_size, 1, commit, false, is_large, stats);
  if (p == NULL) return NULL;
  // and selectively unmap parts around the over-allocated area. (noop on sbrk)
  void* aligned_p = mi_align_up_ptr(p, alignment);
  size_t pre_size = (uint8_t*)aligned_p - (uint8_t*)p;
  size_t mid_size = _mi_align_up(size, _mi_os_page_size());
  size_t post_size = over_size - pre_size - mid_size;
  mi_assert_internal(pre_size < over_size && post_size < over_size && mid_size >= size);
  if (pre_size > 0)  mi_os_mem_free(p, pre_size, commit, stats);
  if (post_size > 0) mi_os_mem_free((uint8_t*)aligned_p + mid_size, post_size, commit, stats);
  // we can return the aligned pointer on `mmap` (and sbrk) systems
  p = aligned_p;
  return p;
}

bool mi_os_mem_free_impl(void* addr, size_t size, bool was_committed) {
  MI_UNUSED(addr);
  MI_UNUSED(size);
  MI_UNUSED(was_committed);
  return false;
}

int mi_os_commitx_impl(void* start, size_t csize, bool commit, bool conservative, bool* is_zero) {
  // WebAssembly guests can't control memory protection
  MI_UNUSED(start);
  MI_UNUSED(csize);
  MI_UNUSED(commit);
  MI_UNUSED(conservative);
  MI_UNUSED(is_zero);
  return 0;
}

bool mi_os_resetx_impl(void* start, size_t csize, bool reset) {
  MI_UNUSED(start);
  MI_UNUSED(csize);
  MI_UNUSED(reset);
  return true;
}

int mi_os_protectx_impl(void* start, size_t csize, bool protect) {
  MI_UNUSED(start);
  MI_UNUSED(csize);
  MI_UNUSED(protect);
  return 0;
}

#endif