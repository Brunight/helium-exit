// helium-exit: LD_PRELOAD shim that moves Helium's "Exit" item from the
// "More tools" submenu back to the top-level app menu (right after Settings).
//
// Linux/x86-64 port of the idea behind github.com/hang333/fuck-helium.
// It scans the main executable's code at startup for the two validated call
// sites, and only patches if exactly one candidate matches (fail-closed).
// Nothing on disk is modified.

#define _GNU_SOURCE
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IDC_EXIT 34031
#define IDC_OPTIONS 40015

typedef void (*add_item_fn)(void *model, int command_id, int string_id,
                            const void *icon);

static add_item_fn g_add_item;
static int g_exit_string_id;
static const void *g_exit_icon;
static int g_exit_pending;

static int g_verbose;
#define LOG(...)                                                    \
  do {                                                              \
    if (g_verbose) {                                                \
      fprintf(stderr, "[helium-exit] " __VA_ARGS__);                \
      fputc('\n', stderr);                                          \
    }                                                               \
  } while (0)

// Called instead of AddItem(..., IDC_EXIT, ...) in the More tools submenu.
static void exit_hook(void *model, int command_id, int string_id,
                      const void *icon) {
  if (command_id != IDC_EXIT) {  // should never happen; be transparent
    g_add_item(model, command_id, string_id, icon);
    return;
  }
  (void)model;
  g_exit_string_id = string_id;
  g_exit_icon = icon;
  g_exit_pending = 1;
}

// Called instead of AddItem(..., IDC_OPTIONS, ...) in the app menu.
static void settings_hook(void *model, int command_id, int string_id,
                          const void *icon) {
  g_add_item(model, command_id, string_id, icon);
  if (command_id == IDC_OPTIONS && g_exit_pending) {
    g_exit_pending = 0;
    g_add_item(model, IDC_EXIT, g_exit_string_id, g_exit_icon);
  }
}

struct text_seg {
  uint8_t *begin;
  size_t size;
};

static int find_exec_segment(struct dl_phdr_info *info, size_t size,
                             void *data) {
  (void)size;
  struct text_seg *seg = data;
  // First object is the main program.
  for (int i = 0; i < info->dlpi_phnum; i++) {
    const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
    if (ph->p_type == PT_LOAD && (ph->p_flags & PF_X)) {
      if (ph->p_memsz > seg->size) {
        seg->begin = (uint8_t *)(info->dlpi_addr + ph->p_vaddr);
        seg->size = ph->p_memsz;
      }
    }
  }
  return 1;  // stop after main program
}

static int32_t rd32(const uint8_t *p) {
  int32_t v;
  memcpy(&v, p, 4);
  return v;
}

static int in_seg(const struct text_seg *t, const uint8_t *p, size_t n) {
  return p >= t->begin && p + n <= t->begin + t->size;
}

// Pattern: BE <cmd32> BA <imm32> E8 <rel32>
//   mov esi, cmd ; mov edx, string_id ; call helper
struct site {
  uint8_t *call;  // address of E8
  uint8_t *target;
  uint8_t *start;  // address of BE
};

static int find_sites(const struct text_seg *t, int cmd, struct site *out,
                      int max) {
  uint8_t pat[5] = {0xBE};
  memcpy(pat + 1, &cmd, 4);
  int n = 0;
  uint8_t *p = t->begin, *end = t->begin + t->size;
  while (p < end) {
    p = memmem(p, end - p, pat, 5);
    if (!p) break;
    if (in_seg(t, p, 15) && p[5] == 0xBA && p[10] == 0xE8) {
      uint8_t *target = p + 15 + rd32(p + 11);
      if (in_seg(t, target, 16) && n < max) {
        out[n++] = (struct site){p + 10, target, p};
      }
    }
    p++;
  }
  return n;
}

// The helper (SimpleMenuModel::AddItemWithStringIdAndIcon-like) prologue.
static int helper_looks_right(const uint8_t *f) {
  static const uint8_t sig[] = {0x55, 0x48, 0x89, 0xE5};  // push rbp; mov rbp,rsp
  return memcmp(f, sig, sizeof sig) == 0;
}

// Within 128 bytes before the Exit site: mov rdi,<reg>; xor esi,esi; call X
// (AddSeparator(NORMAL_SEPARATOR)). Return the last one found.
static uint8_t *find_separator_call(const struct text_seg *t, uint8_t *exit_start) {
  uint8_t *res = NULL;
  if (!in_seg(t, exit_start - 128, 128)) return NULL;
  for (uint8_t *p = exit_start - 128; p + 8 <= exit_start; p++) {
    if (p[0] == 0x48 && p[1] == 0x89 && (p[2] & 0xC7) == 0xC7 &&
        p[3] == 0x31 && p[4] == 0xF6 && p[5] == 0xE8) {
      uint8_t *target = p + 10 + rd32(p + 6);
      if (in_seg(t, target, 1)) res = p + 5;
    }
  }
  return res;
}

// Allocate a page within +-2GB of `near` so rel32 calls can reach it.
static uint8_t *alloc_near(uint8_t *near) {
  long pg = sysconf(_SC_PAGESIZE);
  for (uintptr_t delta = 0x10000000; delta < 0x70000000; delta += 0x1000000) {
    for (int dir = -1; dir <= 1; dir += 2) {
      uintptr_t hint = ((uintptr_t)near + dir * (intptr_t)delta) & ~(uintptr_t)(pg - 1);
      void *m = mmap((void *)hint, pg, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (m != MAP_FAILED) {
        intptr_t d = (intptr_t)((uint8_t *)m - near);
        if (d > -0x7ff00000L && d < 0x7ff00000L) return m;
        munmap(m, pg);
      }
    }
  }
  return NULL;
}

static uint8_t *emit_abs_jmp(uint8_t *p, void *dest) {
  // movabs rax, imm64 ; jmp rax   (rax is not an argument register)
  p[0] = 0x48; p[1] = 0xB8;
  memcpy(p + 2, &dest, 8);
  p[10] = 0xFF; p[11] = 0xE0;
  return p + 12;
}

static int patch_bytes(uint8_t *at, const uint8_t *bytes, size_t n) {
  long pg = sysconf(_SC_PAGESIZE);
  uintptr_t a = (uintptr_t)at & ~(uintptr_t)(pg - 1);
  size_t len = ((uintptr_t)at + n) - a;
  if (mprotect((void *)a, len, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) return 0;
  memcpy(at, bytes, n);
  mprotect((void *)a, len, PROT_READ | PROT_EXEC);
  __builtin___clear_cache((char *)at, (char *)at + n);
  return 1;
}

static void make_call(uint8_t out[5], uint8_t *at, uint8_t *dest) {
  int32_t rel = (int32_t)(dest - (at + 5));
  out[0] = 0xE8;
  memcpy(out + 1, &rel, 4);
}

static int is_helium_executable(void) {
  char exe[4096];
  ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
  if (n <= 0) return 0;
  exe[n] = 0;
  const char *base = strrchr(exe, '/');
  base = base ? base + 1 : exe;
  return strcmp(base, "helium") == 0 || strcmp(base, "chrome") == 0;
}

static int is_browser_process(void) {
  FILE *f = fopen("/proc/self/cmdline", "rb");
  if (!f) return 0;
  char buf[8192];
  size_t n = fread(buf, 1, sizeof buf - 1, f);
  fclose(f);
  buf[n] = 0;
  for (size_t i = 0; i < n; i += strlen(buf + i) + 1)
    if (strncmp(buf + i, "--type=", 7) == 0) return 0;
  return 1;
}

static void strip_self_from_ld_preload(void) {
  const char *v = getenv("LD_PRELOAD");
  if (!v) return;
  char *copy = strdup(v), *out = calloc(1, strlen(v) + 1), *save = NULL;
  for (char *tok = strtok_r(copy, ": ", &save); tok; tok = strtok_r(NULL, ": ", &save)) {
    if (strstr(tok, "helium_exit")) continue;
    if (*out) strcat(out, ":");
    strcat(out, tok);
  }
  if (*out) setenv("LD_PRELOAD", out, 1); else unsetenv("LD_PRELOAD");
  free(copy);
  free(out);
}

__attribute__((constructor)) static void helium_exit_init(void) {
  g_verbose = getenv("HELIUM_EXIT_DEBUG") != NULL;
  // Wrapper scripts (bash) also load us: stay inert and keep LD_PRELOAD
  // intact so it reaches Helium.
  if (!is_helium_executable()) return;
  // Inside Helium: children (renderer, gpu, zygote...) don't need us.
  strip_self_from_ld_preload();
  if (!is_browser_process()) return;

  struct text_seg t = {0};
  dl_iterate_phdr(find_exec_segment, &t);
  if (!t.begin) { LOG("no executable segment"); return; }

  struct site exits[8], opts[8];
  int ne = find_sites(&t, IDC_EXIT, exits, 8);
  int no = find_sites(&t, IDC_OPTIONS, opts, 8);

  int found = 0;
  struct site ex = {0}, op = {0};
  uint8_t *sep = NULL;
  for (int i = 0; i < ne; i++) {
    if (!helper_looks_right(exits[i].target)) continue;
    uint8_t *s = find_separator_call(&t, exits[i].start);
    if (!s) continue;
    for (int j = 0; j < no; j++) {
      if (opts[j].target == exits[i].target) {
        ex = exits[i]; op = opts[j]; sep = s; found++;
      }
    }
  }
  if (found != 1) {
    LOG("patch skipped: expected 1 Exit/Settings pair, found %d (exit=%d settings=%d)",
        found, ne, no);
    return;
  }

  uint8_t *tramp = alloc_near(t.begin);
  if (!tramp) { LOG("could not allocate near trampoline"); return; }
  uint8_t *exit_stub = tramp, *settings_stub = tramp + 16;
  emit_abs_jmp(exit_stub, (void *)exit_hook);
  emit_abs_jmp(settings_stub, (void *)settings_hook);
  if (mprotect(tramp, sysconf(_SC_PAGESIZE), PROT_READ | PROT_EXEC) != 0) {
    LOG("mprotect trampoline failed");
    return;
  }
  g_add_item = (add_item_fn)ex.target;

  static const uint8_t nop5[5] = {0x0F, 0x1F, 0x44, 0x00, 0x00};
  uint8_t c1[5], c2[5];
  make_call(c1, ex.call, exit_stub);
  make_call(c2, op.call, settings_stub);
  uint8_t sep_orig[5];
  memcpy(sep_orig, sep, 5);
  if (!patch_bytes(sep, nop5, 5)) { LOG("patch separator failed"); return; }
  if (!patch_bytes(op.call, c2, 5)) {
    patch_bytes(sep, sep_orig, 5);
    LOG("patch settings failed");
    return;
  }
  if (!patch_bytes(ex.call, c1, 5)) {
    // settings hook is harmless without exit hook (pending never set)
    patch_bytes(sep, sep_orig, 5);
    LOG("patch exit failed");
    return;
  }
  LOG("patched: helper=%p exit_call=%p settings_call=%p separator=%p", (void *)ex.target,
      (void *)ex.call, (void *)op.call, (void *)sep);
}
