#if defined(_WIN32) && defined(MI_SHARED_LIB)
#ifdef __cplusplus
extern "C" {
#endif
mi_decl_export void _mi_redirect_entry(DWORD reason) {
  // called on redirection; careful as this may be called before DllMain
  if (reason == DLL_PROCESS_ATTACH) {
    mi_redirected = true;
  }
  else if (reason == DLL_PROCESS_DETACH) {
    mi_redirected = false;
  }
  else if (reason == DLL_THREAD_DETACH) {
    mi_thread_done();
  }
}
__declspec(dllimport) bool mi_allocator_init(const char** message);
__declspec(dllimport) void mi_allocator_done(void);
#ifdef __cplusplus
}
#endif
#endif

#if defined(_WIN32) && defined(MI_SHARED_LIB)
  // Windows DLL: easy to hook into process_init and thread_done
  __declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved) {
    MI_UNUSED(reserved);
    MI_UNUSED(inst);
    if (reason==DLL_PROCESS_ATTACH) {
      mi_process_load();
    }
    else if (reason==DLL_PROCESS_DETACH) {
      mi_process_done();
    }
    else if (reason==DLL_THREAD_DETACH) {
      if (!mi_is_redirected()) {
        mi_thread_done();
      }
    }    
    return TRUE;
  }
#elif defined(_MSC_VER)
  // MSVC: use data section magic for static libraries
  // See <https://www.codeguru.com/cpp/misc/misc/applicationcontrol/article.php/c6945/Running-Code-Before-and-After-Main.htm>
  static int _mi_process_init(void) {
    mi_process_load();
    return 0;
  }
  typedef int(*_mi_crt_callback_t)(void);
  #if defined(_M_X64) || defined(_M_ARM64)
    __pragma(comment(linker, "/include:" "_mi_msvc_initu"))
    #pragma section(".CRT$XIU", long, read)
  #else
    __pragma(comment(linker, "/include:" "__mi_msvc_initu"))
  #endif
  #pragma data_seg(".CRT$XIU")
  mi_decl_externc _mi_crt_callback_t _mi_msvc_initu[] = { &_mi_process_init };
  #pragma data_seg()
#endif