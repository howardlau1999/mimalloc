#if !defined(_WIN32) && !defined(__wasi__)

#include "os.h" 
#include <sys/mman.h>  // mmap
#include <unistd.h>    // sysconf
#if defined(__linux__)
#include <features.h>
#include <fcntl.h>
#if defined(__GLIBC__)
#include <linux/mman.h> // linux mmap flags
#else
#include <sys/mman.h>
#endif
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if !TARGET_IOS_IPHONE && !TARGET_IOS_SIMULATOR
#include <mach/vm_statistics.h>
#endif
#endif
#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/param.h>
#if __FreeBSD_version >= 1200000
#include <sys/cpuset.h>
#include <sys/domainset.h>
#endif
#include <sys/sysctl.h>
#endif

#if defined(MADV_NORMAL)
static int mi_madvise(void* addr, size_t length, int advice) {
  #if defined(__sun)
  return madvise((caddr_t)addr, length, advice);  // Solaris needs cast (issue #520)
  #else
  return madvise(addr, length, advice);
  #endif
}
#endif

#if defined(MI_USE_SBRK) 
static void* mi_memory_grow( size_t size ) {
  void* p = sbrk(size);
  if (p == (void*)(-1)) return NULL;
  #if !defined(__wasi__) // on wasi this is always zero initialized already (?)
  memset(p,0,size); 
  #endif
  return p;
}

static void* mi_heap_grow(size_t size, size_t try_alignment) {
  void* p = NULL;
  if (try_alignment <= 1) {
    // `sbrk` is not thread safe in general so try to protect it (we could skip this on WASM but leave it in for now)
    #if defined(MI_USE_PTHREADS) 
    pthread_mutex_lock(&mi_heap_grow_mutex);
    #endif
    p = mi_memory_grow(size);
    #if defined(MI_USE_PTHREADS)
    pthread_mutex_unlock(&mi_heap_grow_mutex);
    #endif
  }
  else {
    void* base = NULL;
    size_t alloc_size = 0;
    // to allocate aligned use a lock to try to avoid thread interaction
    // between getting the current size and actual allocation
    // (also, `sbrk` is not thread safe in general)
    #if defined(MI_USE_PTHREADS)
    pthread_mutex_lock(&mi_heap_grow_mutex);
    #endif
    {
      void* current = mi_memory_grow(0);  // get current size
      if (current != NULL) {
        void* aligned_current = mi_align_up_ptr(current, try_alignment);  // and align from there to minimize wasted space
        alloc_size = _mi_align_up( ((uint8_t*)aligned_current - (uint8_t*)current) + size, _mi_os_page_size());
        base = mi_memory_grow(alloc_size);        
      }
    }
    #if defined(MI_USE_PTHREADS)
    pthread_mutex_unlock(&mi_heap_grow_mutex);
    #endif
    if (base != NULL) {
      p = mi_align_up_ptr(base, try_alignment);
      if ((uint8_t*)p + size > (uint8_t*)base + alloc_size) {
        // another thread used wasm_memory_grow/sbrk in-between and we do not have enough
        // space after alignment. Give up (and waste the space as we cannot shrink :-( )
        // (in `mi_os_mem_alloc_aligned` this will fall back to overallocation to align)
        p = NULL;
      }
    }
  }
  if (p == NULL) {
    _mi_warning_message("unable to allocate sbrk/wasm_memory_grow OS memory (%zu bytes, %zu alignment)\n", size, try_alignment);    
    errno = ENOMEM;
    return NULL;
  }
  mi_assert_internal( try_alignment == 0 || (uintptr_t)p % try_alignment == 0 );
  return p;
}
#endif

static void os_detect_overcommit(void) {
#if defined(__linux__)
  int fd = open("/proc/sys/vm/overcommit_memory", O_RDONLY);
	if (fd < 0) return;
  char buf[32];
  ssize_t nread = read(fd, &buf, sizeof(buf));
	close(fd);
  // <https://www.kernel.org/doc/Documentation/vm/overcommit-accounting>
  // 0: heuristic overcommit, 1: always overcommit, 2: never overcommit (ignore NORESERVE)
  if (nread >= 1) {
    os_overcommit = (buf[0] == '0' || buf[0] == '1');
  }
#elif defined(__FreeBSD__)
  int val = 0;
  size_t olen = sizeof(val);
  if (sysctlbyname("vm.overcommit", &val, &olen, NULL, 0) == 0) {
    os_overcommit = (val != 0);
  }  
#else
  // default: overcommit is true  
#endif
}

void _mi_os_init(void) {
  // get the page size
  long result = sysconf(_SC_PAGESIZE);
  if (result > 0) {
    os_page_size = (size_t)result;
    os_alloc_granularity = os_page_size;
  }
  large_os_page_size = 2*MI_MiB; // TODO: can we query the OS for this?
  os_detect_overcommit();
}

#if !defined(MI_USE_SBRK)

/* -----------------------------------------------------------
  Raw allocation on Unix's (mmap)
-------------------------------------------------------------- */

#define MI_OS_USE_MMAP

static void* mi_unix_mmapx(void* addr, size_t size, size_t try_alignment, int protect_flags, int flags, int fd) {
  MI_UNUSED(try_alignment);  
  #if defined(MAP_ALIGNED)  // BSD
  if (addr == NULL && try_alignment > 1 && (try_alignment % _mi_os_page_size()) == 0) {
    size_t n = mi_bsr(try_alignment);
    if (((size_t)1 << n) == try_alignment && n >= 12 && n <= 30) {  // alignment is a power of 2 and 4096 <= alignment <= 1GiB
      flags |= MAP_ALIGNED(n);
      void* p = mmap(addr, size, protect_flags, flags | MAP_ALIGNED(n), fd, 0);
      if (p!=MAP_FAILED) return p;
      // fall back to regular mmap
    }
  }
  #elif defined(MAP_ALIGN)  // Solaris
  if (addr == NULL && try_alignment > 1 && (try_alignment % _mi_os_page_size()) == 0) {
    void* p = mmap((void*)try_alignment, size, protect_flags, flags | MAP_ALIGN, fd, 0);  // addr parameter is the required alignment
    if (p!=MAP_FAILED) return p;
    // fall back to regular mmap
  }
  #endif
  #if (MI_INTPTR_SIZE >= 8) && !defined(MAP_ALIGNED)
  // on 64-bit systems, use the virtual address area after 2TiB for 4MiB aligned allocations
  if (addr == NULL) {
    void* hint = mi_os_get_aligned_hint(try_alignment, size);
    if (hint != NULL) {
      void* p = mmap(hint, size, protect_flags, flags, fd, 0);
      if (p!=MAP_FAILED) return p;
      // fall back to regular mmap
    }
  }
  #endif
  // regular mmap
  void* p = mmap(addr, size, protect_flags, flags, fd, 0);
  if (p!=MAP_FAILED) return p;  
  // failed to allocate
  return NULL;
}

static int mi_unix_mmap_fd(void) {
#if defined(VM_MAKE_TAG)
  // macOS: tracking anonymous page with a specific ID. (All up to 98 are taken officially but LLVM sanitizers had taken 99)
  int os_tag = (int)mi_option_get(mi_option_os_tag);
  if (os_tag < 100 || os_tag > 255) os_tag = 100;
  return VM_MAKE_TAG(os_tag);
#else
  return -1;
#endif
}

static void* mi_unix_mmap(void* addr, size_t size, size_t try_alignment, int protect_flags, bool large_only, bool allow_large, bool* is_large) {
  void* p = NULL;
  #if !defined(MAP_ANONYMOUS)
  #define MAP_ANONYMOUS  MAP_ANON
  #endif
  #if !defined(MAP_NORESERVE)
  #define MAP_NORESERVE  0
  #endif
  const int fd = mi_unix_mmap_fd();
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (_mi_os_has_overcommit()) {
    flags |= MAP_NORESERVE;
  }  
  #if defined(PROT_MAX)
  protect_flags |= PROT_MAX(PROT_READ | PROT_WRITE); // BSD
  #endif    
  // huge page allocation
  if ((large_only || use_large_os_page(size, try_alignment)) && allow_large) {
    static _Atomic(size_t) large_page_try_ok; // = 0;
    size_t try_ok = mi_atomic_load_acquire(&large_page_try_ok);
    if (!large_only && try_ok > 0) {
      // If the OS is not configured for large OS pages, or the user does not have
      // enough permission, the `mmap` will always fail (but it might also fail for other reasons).
      // Therefore, once a large page allocation failed, we don't try again for `large_page_try_ok` times
      // to avoid too many failing calls to mmap.
      mi_atomic_cas_strong_acq_rel(&large_page_try_ok, &try_ok, try_ok - 1);
    }
    else {
      int lflags = flags & ~MAP_NORESERVE;  // using NORESERVE on huge pages seems to fail on Linux
      int lfd = fd;
      #ifdef MAP_ALIGNED_SUPER
      lflags |= MAP_ALIGNED_SUPER;
      #endif
      #ifdef MAP_HUGETLB
      lflags |= MAP_HUGETLB;
      #endif
      #ifdef MAP_HUGE_1GB
      static bool mi_huge_pages_available = true;
      if ((size % MI_GiB) == 0 && mi_huge_pages_available) {
        lflags |= MAP_HUGE_1GB;
      }
      else
      #endif
      {
        #ifdef MAP_HUGE_2MB
        lflags |= MAP_HUGE_2MB;
        #endif
      }
      #ifdef VM_FLAGS_SUPERPAGE_SIZE_2MB
      lfd |= VM_FLAGS_SUPERPAGE_SIZE_2MB;
      #endif
      if (large_only || lflags != flags) {
        // try large OS page allocation
        *is_large = true;
        p = mi_unix_mmapx(addr, size, try_alignment, protect_flags, lflags, lfd);
        #ifdef MAP_HUGE_1GB
        if (p == NULL && (lflags & MAP_HUGE_1GB) != 0) {
          mi_huge_pages_available = false; // don't try huge 1GiB pages again
          _mi_warning_message("unable to allocate huge (1GiB) page, trying large (2MiB) pages instead (error %i)\n", errno);
          lflags = ((lflags & ~MAP_HUGE_1GB) | MAP_HUGE_2MB);
          p = mi_unix_mmapx(addr, size, try_alignment, protect_flags, lflags, lfd);
        }
        #endif
        if (large_only) return p;
        if (p == NULL) {
          mi_atomic_store_release(&large_page_try_ok, (size_t)8);  // on error, don't try again for the next N allocations
        }
      }
    }
  }
  // regular allocation
  if (p == NULL) {
    *is_large = false;
    p = mi_unix_mmapx(addr, size, try_alignment, protect_flags, flags, fd);
    if (p != NULL) {
      #if defined(MADV_HUGEPAGE)
      // Many Linux systems don't allow MAP_HUGETLB but they support instead
      // transparent huge pages (THP). Generally, it is not required to call `madvise` with MADV_HUGE
      // though since properly aligned allocations will already use large pages if available
      // in that case -- in particular for our large regions (in `memory.c`).
      // However, some systems only allow THP if called with explicit `madvise`, so
      // when large OS pages are enabled for mimalloc, we call `madvise` anyways.
      if (allow_large && use_large_os_page(size, try_alignment)) {
        if (mi_madvise(p, size, MADV_HUGEPAGE) == 0) {
          *is_large = true; // possibly
        };
      }
      #elif defined(__sun)
      if (allow_large && use_large_os_page(size, try_alignment)) {
        struct memcntl_mha cmd = {0};
        cmd.mha_pagesize = large_os_page_size;
        cmd.mha_cmd = MHA_MAPSIZE_VA;
        if (memcntl((caddr_t)p, size, MC_HAT_ADVISE, (caddr_t)&cmd, 0, 0) == 0) {
          *is_large = true;
        }
      }      
      #endif
    }
  }
  if (p == NULL) {
    _mi_warning_message("unable to allocate OS memory (%zu bytes, error code: %i, address: %p, large only: %d, allow large: %d)\n", size, errno, addr, large_only, allow_large);
  }
  return p;
}

#endif

void* mi_os_mem_alloc_impl(size_t size, size_t try_alignment, bool commit, bool allow_large, bool* is_large) {
  int protect_flags = (commit ? (PROT_WRITE | PROT_READ) : PROT_NONE);
  return mi_unix_mmap(NULL, size, try_alignment, protect_flags, false, allow_large, is_large);
}

void* mi_os_mem_alloc_aligned_impl(size_t over_size, size_t size, size_t alignment, bool commit, bool allow_large, bool* is_large, mi_stats_t* stats) { 
  MI_UNUSED(allow_large);
  MI_UNUSED(is_large);
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
  MI_UNUSED(was_committed);
  #if defined(MI_USE_SBRK)
  return false;
  #endif
  bool err = (munmap(addr, size) == -1);
  if (err) {
    _mi_warning_message("unable to release OS memory: %s, addr: %p, size: %zu\n", strerror(errno), addr, size);
  }
  return err;
}

int mi_os_commitx_impl(void* start, size_t csize, bool commit, bool conservative, bool* is_zero) {
  MI_UNUSED(conservative);
  MI_UNUSED(is_zero);
  int err = 0;
  if (commit) {
    // commit: ensure we can access the area    
    err = mprotect(start, csize, (PROT_READ | PROT_WRITE));
    if (err != 0) { err = errno; }
  } 
  else {
    #if defined(MADV_DONTNEED) && MI_DEBUG == 0 && MI_SECURE == 0
    // decommit: use MADV_DONTNEED as it decreases rss immediately (unlike MADV_FREE)
    // (on the other hand, MADV_FREE would be good enough.. it is just not reflected in the stats :-( )
    err = madvise(start, csize, MADV_DONTNEED);
    #else
    // decommit: just disable access (also used in debug and secure mode to trap on illegal access)
    err = mprotect(start, csize, PROT_NONE);
    if (err != 0) { err = errno; }
    #endif
    //#if defined(MADV_FREE_REUSE)
    //  while ((err = mi_madvise(start, csize, MADV_FREE_REUSE)) != 0 && errno == EAGAIN) { errno = 0; }
    //#endif
  }
  return err;
}

bool mi_os_resetx_impl(void* start, size_t csize, bool reset) {
  MI_UNUSED(reset);
#if defined(MADV_FREE)
  static _Atomic(size_t) advice = MI_ATOMIC_VAR_INIT(MADV_FREE);
  int oadvice = (int)mi_atomic_load_relaxed(&advice);
  int err;
  while ((err = mi_madvise(start, csize, oadvice)) != 0 && errno == EAGAIN) { errno = 0;  };
  if (err != 0 && errno == EINVAL && oadvice == MADV_FREE) {  
    // if MADV_FREE is not supported, fall back to MADV_DONTNEED from now on
    mi_atomic_store_release(&advice, (size_t)MADV_DONTNEED);
    err = mi_madvise(start, csize, MADV_DONTNEED);
  }
#else
  int err = mi_madvise(start, csize, MADV_DONTNEED);
#endif
  if (err != 0) {
    _mi_warning_message("madvise reset error: start: %p, csize: 0x%zx, errno: %i\n", start, csize, errno);
  }
  //mi_assert(err == 0);
  return err == 0;
}

int mi_os_protectx_impl(void* start, size_t csize, bool protect) {
  int err = mprotect(start, csize, protect ? PROT_NONE : (PROT_READ | PROT_WRITE));
  if (err != 0) { err = errno; }
  return err;
}

#if defined(MI_OS_USE_MMAP) && (MI_INTPTR_SIZE >= 8) && !defined(__HAIKU__)
#include <sys/syscall.h>
#ifndef MPOL_PREFERRED
#define MPOL_PREFERRED 1
#endif
#if defined(SYS_mbind)
static long mi_os_mbind(void* start, unsigned long len, unsigned long mode, const unsigned long* nmask, unsigned long maxnode, unsigned flags) {
  return syscall(SYS_mbind, start, len, mode, nmask, maxnode, flags);
}
#else
static long mi_os_mbind(void* start, unsigned long len, unsigned long mode, const unsigned long* nmask, unsigned long maxnode, unsigned flags) {
  MI_UNUSED(start); MI_UNUSED(len); MI_UNUSED(mode); MI_UNUSED(nmask); MI_UNUSED(maxnode); MI_UNUSED(flags);
  return 0;
}
#endif
void* mi_os_alloc_huge_os_pagesx_impl(void* addr, size_t size, int numa_node) {
  mi_assert_internal(size%MI_GiB == 0);
  bool is_large = true;
  void* p = mi_unix_mmap(addr, size, MI_SEGMENT_SIZE, PROT_READ | PROT_WRITE, true, true, &is_large);
  if (p == NULL) return NULL;
  if (numa_node >= 0 && numa_node < 8*MI_INTPTR_SIZE) { // at most 64 nodes
    unsigned long numa_mask = (1UL << numa_node);
    // TODO: does `mbind` work correctly for huge OS pages? should we
    // use `set_mempolicy` before calling mmap instead?
    // see: <https://lkml.org/lkml/2017/2/9/875>
    long err = mi_os_mbind(p, size, MPOL_PREFERRED, &numa_mask, 8*MI_INTPTR_SIZE, 0);
    if (err != 0) {
      _mi_warning_message("failed to bind huge (1GiB) pages to numa node %d: %s\n", numa_node, strerror(errno));
    }
  }
  return p;
}
#endif

#if defined(__linux__)
#include <sys/syscall.h>  // getcpu
#include <stdio.h>        // access

size_t mi_os_numa_nodex(void) {
#ifdef SYS_getcpu
  unsigned long node = 0;
  unsigned long ncpu = 0;
  long err = syscall(SYS_getcpu, &ncpu, &node, NULL);
  if (err != 0) return 0;
  return node;
#else
  return 0;
#endif
}
size_t mi_os_numa_node_countx(void) {
  char buf[128];
  unsigned node = 0;
  for(node = 0; node < 256; node++) {
    // enumerate node entries -- todo: it there a more efficient way to do this? (but ensure there is no allocation)
    snprintf(buf, 127, "/sys/devices/system/node/node%u", node + 1);
    if (access(buf,R_OK) != 0) break;
  }
  return (node+1);
}
#elif defined(__FreeBSD__) && __FreeBSD_version >= 1200000
size_t mi_os_numa_nodex(void) {
  domainset_t dom;
  size_t node;
  int policy;
  if (cpuset_getdomain(CPU_LEVEL_CPUSET, CPU_WHICH_PID, -1, sizeof(dom), &dom, &policy) == -1) return 0ul;
  for (node = 0; node < MAXMEMDOM; node++) {
    if (DOMAINSET_ISSET(node, &dom)) return node;
  }
  return 0ul;
}
size_t mi_os_numa_node_countx(void) {
  size_t ndomains = 0;
  size_t len = sizeof(ndomains);
  if (sysctlbyname("vm.ndomains", &ndomains, &len, NULL, 0) == -1) return 0ul;
  return ndomains;
}
#elif defined(__DragonFly__)
size_t mi_os_numa_nodex(void) {
  // TODO: DragonFly does not seem to provide any userland means to get this information.
  return 0ul;
}
size_t mi_os_numa_node_countx(void) {
  size_t ncpus = 0, nvirtcoresperphys = 0;
  size_t len = sizeof(size_t);
  if (sysctlbyname("hw.ncpu", &ncpus, &len, NULL, 0) == -1) return 0ul;
  if (sysctlbyname("hw.cpu_topology_ht_ids", &nvirtcoresperphys, &len, NULL, 0) == -1) return 0ul;
  return nvirtcoresperphys * ncpus;
}
#else
size_t mi_os_numa_nodex(void) {
  return 0;
}
size_t mi_os_numa_node_countx(void) {
  return 1;
}
#endif

#endif