/*
 * POD Gold Recompilation - import bridge helpers.
 *
 * Shims run inside the recomp register/stack model. When the lifted code calls
 * an import, RECOMP_ICALL has already pushed a dummy return address, so the
 * stack from ESP looks like: [ret][arg0][arg1]... Each arg is one 32-bit slot.
 *
 *   ARG(n)     read argument n (0-based)
 *   ARGP(n,T)  read argument n as a host pointer of type T* (maps VA->host)
 *   RET(v)     set the return value (EAX)
 *   STDRET(k)  stdcall epilogue: pop the dummy ret addr + k argument slots
 */
#ifndef POD_IMPORTS_H
#define POD_IMPORTS_H

#include "recomp_types.h"

#define ARG(n)      MEM32(g_esp + 4 + (n)*4)
#define ARGP(n, T)  ((T*)(void*)(uintptr_t)ADDR(ARG(n)))
#define RET(v)      do { g_eax = (uint32_t)(v); } while (0)
#define STDRET(k)   do { g_esp += 4 + (uint32_t)(k)*4; } while (0)

/* Log an unimplemented import once, by name. */
#define IMPORT_STUB(nm) do { \
    static int _w = 0; \
    if (!_w) { fprintf(stderr, "[import-stub] %s\n", (nm)); _w = 1; } \
} while (0)

/* Simulated-memory allocators (recomp_runtime.c). Return VAs. */
uint32_t shim_alloc(uint32_t n);
uint32_t shim_alloc_aligned(uint32_t n, uint32_t align);
uint32_t shim_strdup(const char* s);
uint32_t host_millis(void);

/* Host file + INI I/O (recomp_runtime.c), backed by the real game directory. */
void     host_translate_path(uint32_t va, char* out, int outsz);
uint32_t host_file_open(uint32_t name_va, uint32_t access, uint32_t disp);
uint32_t host_file_read(uint32_t sh, uint32_t buf_va, uint32_t n);
uint32_t host_file_write(uint32_t sh, uint32_t buf_va, uint32_t n);
uint32_t host_file_seek(uint32_t sh, int32_t dist, uint32_t method);
uint32_t host_file_size(uint32_t sh);
void     host_file_close(uint32_t sh);
int      host_file_getc(uint32_t sh);
int      host_is_file(uint32_t sh);
uint32_t host_fopen(uint32_t name_va, uint32_t mode_va);
uint32_t host_stream_handle(uint32_t stream_va);
void     host_stream_set_eof(uint32_t stream_va, int eof);
uint32_t host_ini_string(uint32_t sect, uint32_t key, uint32_t def, uint32_t buf, uint32_t bufsz, uint32_t file);
uint32_t host_ini_int(uint32_t sect, uint32_t key, uint32_t def, uint32_t file);
uint32_t host_ini_write(uint32_t sect, uint32_t key, uint32_t val, uint32_t file);
uint32_t host_stat(uint32_t name_va, uint32_t buf_va);
void     host_index_build(void);
uint32_t host_filelength(uint32_t name_va);
uint32_t host_malloc(uint32_t n);
void     host_free(uint32_t ptr);
uint32_t host_realloc(uint32_t ptr, uint32_t n);
uint32_t host_calloc(uint32_t num, uint32_t sz);

/* Display (recomp_runtime.c). */
uint32_t host_create_window(uint32_t title_va);
uint32_t host_create_dib(uint32_t pbmi_va, uint32_t ppvbits_va);
void     host_set_dib_colors(uint32_t start, uint32_t n, uint32_t prgb_va);
void     host_blit(void);
int      host_pump(void);
void     host_window_ensure(int w, int h);
void     host_present16(uint32_t buf_va, int w, int h, int pitch);
void     host_register_surface16(uint32_t buf, int w, int h, int pitch);

/* DirectDraw COM shim (ddraw_shim.c). DirectDrawCreate builds a fake IDirectDraw
 * whose vtable entries are synthetic VAs; pod_com_lookup resolves those VAs to the
 * host method shims (checked by recomp_lookup_import). */
void          imp_DirectDrawCreate(void);
void          imp_DirectInputCreateA(void);
recomp_func_t pod_com_lookup(uint32_t va);
uint32_t      pod_arena_size(void);

/* Threading + sync (thread_shim.c). */
int  pod_obj_is_handle(uint32_t gh);
void pod_obj_close(uint32_t gh);

/* Diagnostics (recomp_runtime.c, RECOMP_TRACE). */
void recomp_dump_tls_ring(const char* why);
void recomp_dump_all_rings(const char* why);

/* Generated bridge tables (imports_gen.c). */
extern const recomp_dispatch_entry_t pod_import_bridges[];
extern const uint32_t pod_import_bridge_count;
extern const uint32_t pod_iat_slots[];
extern const uint32_t pod_iat_slot_count;

/* One-shot PAGE_GUARD watchpoint: names the code that next touches this VA. */
void host_watch_page(uint32_t va);
void host_watch_tick(void);
void host_watch_va(uint32_t va, uint32_t len, uint32_t after_ms);
int  host_key_state(uint32_t buf, uint32_t n);
void host_keys_init(void);
void host_keys_poll(void);

#endif /* POD_IMPORTS_H */
