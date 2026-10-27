/* SPDX-License-Identifier: CC0-1.0
 *
 * What the PE loader does with an image, by the caller's options: whose
 * functions its imports are bound to, and whether its entry point runs.
 * The image is tests/peprobe/peprobe.c.
 */
#include "check.h"
#include "pe_loader.h"

#if defined(__x86_64__)
#define WIDTH "linux64"
#else
#define WIDTH "linux32"
#endif

typedef CDECL_CC int (*int_fn)(int);
typedef CDECL_CC int (*void_fn)(void);

static char probe[512];

static const char *probe_path(void)
{
  const char *dir = getenv("SCVA_TEST_PEPROBE_DIR");
  snprintf(probe, sizeof probe, "%s/" WIDTH "/peprobe.dll",
           dir ? dir : "build/test/peprobe");
  return probe;
}

static CDECL_CC int doubled(int x) { return 2 * x; }

static const struct pe_binding hooks[] = {
  {"probe_hook", (void *)doubled},
  {NULL, NULL}
};

/* What the loader asked for, by a resolver that records it. */
static char asked_dll[64], asked_name[64];

static void *recording(void *ctx, const char *dll, const char *name)
{
  snprintf(asked_dll, sizeof asked_dll, "%s", dll ? dll : "(none)");
  snprintf(asked_name, sizeof asked_name, "%s", name);
  return pe_resolve_table(ctx, dll, name);
}

static struct pe_image *load(pe_resolve_fn resolve, const void *ctx, int no_entry,
                             char *err, size_t errlen)
{
  struct pe_options opt;
  opt.resolve = resolve;
  opt.ctx = (void *)ctx;
  opt.no_entry = no_entry;
  return pe_load(probe_path(), &opt, err, errlen);
}

static int call_int(struct pe_image *img, const char *fn, int x)
{
  int_fn f = (int_fn)pe_symbol(img, fn);
  return f ? f(x) : -1;
}

static int call_void(struct pe_image *img, const char *fn)
{
  void_fn f = (void_fn)pe_symbol(img, fn);
  return f ? f() : -1;
}

TEST(imports_call_what_the_resolver_bound_them_to)
{
  char err[256] = "";
  struct pe_image *img = load(pe_resolve_table, hooks, 0, err, sizeof err);
  CHECK(img != NULL);
  if (!img) { fprintf(stderr, "  %s\n", err); return; }
  CHECK_INT(call_int(img, "probe_call", 21), 42);
  pe_unload(img);
}

TEST(the_resolver_is_told_the_library_and_the_function)
{
  char err[256] = "";
  struct pe_image *img = load(recording, hooks, 0, err, sizeof err);
  CHECK(img != NULL);
  CHECK_STR(asked_name, "probe_hook");
  CHECK_STR(asked_dll, "probe_dep.dll");
  pe_unload(img);
}

TEST(an_import_nothing_binds_fails_the_load_and_names_it)
{
  static const struct pe_binding none[] = { {NULL, NULL} };
  char err[256] = "";
  struct pe_image *img = load(pe_resolve_table, none, 0, err, sizeof err);
  CHECK(img == NULL);
  CHECK_STR(err, "no binding for probe_hook (probe_dep.dll)");
}

TEST(the_entry_point_runs_unless_the_caller_says_not_to)
{
  char err[256] = "";
  struct pe_image *img = load(pe_resolve_table, hooks, 0, err, sizeof err);
  CHECK(img && call_void(img, "probe_entry_ran") == 1);
  pe_unload(img);
  img = load(pe_resolve_table, hooks, 1, err, sizeof err);
  CHECK(img && call_void(img, "probe_entry_ran") == 0);
  pe_unload(img);
}

TEST(a_table_binds_by_name_whatever_the_library)
{
  CHECK(pe_resolve_table((void *)hooks, "anything.dll", "probe_hook") == (void *)doubled);
  CHECK(pe_resolve_table((void *)hooks, NULL, "probe_hook") == (void *)doubled);
  CHECK(pe_resolve_table((void *)hooks, "probe_dep.dll", "probe_other") == NULL);
}

TEST(a_missing_file_is_named)
{
  struct pe_options opt;
  char err[256] = "";
  opt.resolve = pe_resolve_table;
  opt.ctx = (void *)hooks;
  opt.no_entry = 0;
  CHECK(pe_load("/nonexistent/x.dll", &opt, err, sizeof err) == NULL);
  CHECK_STR(err, "cannot open /nonexistent/x.dll");
}

/* ---- COFF symbols ---------------------------------------------------------- */

/* C names carry a leading underscore on 32-bit Windows. */
#if defined(__i386__)
#define C_NAME(n) "_" n
#else
#define C_NAME(n) n
#endif

static int read_int(const void *p) { int v = 0; if (p) memcpy(&v, p, sizeof v); return v; }

static struct pe_image *probe_image(void)
{
  char err[256] = "";
  struct pe_image *img = load(pe_resolve_table, hooks, 1, err, sizeof err);
  if (!img) fprintf(stderr, "  %s\n", err);
  return img;
}

TEST(an_external_symbol_is_found_by_its_long_name)
{
  struct pe_image *img = probe_image();
  if (!img) { CHECK(img); return; }
  CHECK_INT(read_int(pe_symbol_coff(img, C_NAME("probe_external"), PE_SYM_EXTERNAL,
                                    PE_COFF_SECTION)), 0x1234);
  pe_unload(img);
}

TEST(an_external_symbol_is_found_by_a_name_short_enough_to_be_inline)
{
  struct pe_image *img = probe_image();
  if (!img) { CHECK(img); return; }
  CHECK_INT(read_int(pe_symbol_coff(img, C_NAME("pshort"), PE_SYM_EXTERNAL,
                                    PE_COFF_SECTION)), 0x77);
  pe_unload(img);
}

TEST(a_file_static_is_found_as_a_static_and_only_as_one)
{
  struct pe_image *img = probe_image();
  if (!img) { CHECK(img); return; }
  CHECK_INT(read_int(pe_symbol_coff(img, C_NAME("probe_static"), PE_SYM_STATIC,
                                    PE_COFF_SECTION)), 0x5678);
  CHECK(pe_symbol_coff(img, C_NAME("probe_static"), PE_SYM_EXTERNAL, PE_COFF_SECTION) == NULL);
  CHECK(pe_symbol_coff(img, C_NAME("probe_external"), PE_SYM_STATIC, PE_COFF_SECTION) == NULL);
  pe_unload(img);
}

TEST(a_name_is_matched_whole)
{
  struct pe_image *img = probe_image();
  if (!img) { CHECK(img); return; }
  CHECK(pe_symbol_coff(img, C_NAME("probe_extern"), PE_SYM_EXTERNAL, PE_COFF_SECTION) == NULL);
  CHECK(pe_symbol_coff(img, C_NAME("pshor"), PE_SYM_EXTERNAL, PE_COFF_SECTION) == NULL);
  CHECK(pe_symbol_coff(img, C_NAME("pshortest"), PE_SYM_EXTERNAL, PE_COFF_SECTION) == NULL);
  CHECK(pe_symbol_coff(img, "no_such_symbol", PE_SYM_EXTERNAL, PE_COFF_SECTION) == NULL);
  pe_unload(img);
}

/* The user's own vsc.sys, a 32-bit driver whose COFF values are
   image-relative, mapped without its entry point and with every import
   bound to a function that is never called. */
#if defined(__i386__)
static CDECL_CC int never_called(void) { abort(); }

static void *bind_anything(void *ctx, const char *dll, const char *name)
{
  (void)ctx; (void)dll; (void)name;
  return (void *)never_called;
}
#endif

TEST(vsc_sys_maps_and_its_entry_points_resolve)
{
#if defined(__i386__)
  static const char *const entries[] = {
    "_tgInit@8", "_FillAudioBuffer@8", "_InitSynth@0", "_RxMidiMsg@4"
  };
  const char *path = getenv("SCVA_TEST_VSC_SYS");
  struct pe_options opt;
  struct pe_image *img;
  char err[256] = "";
  size_t i;
  if (!path) SKIP("set SCVA_TEST_VSC_SYS to a vsc.sys");
  opt.resolve = bind_anything;
  opt.ctx = NULL;
  opt.no_entry = 1;
  img = pe_load(path, &opt, err, sizeof err);
  CHECK(img != NULL);
  if (!img) { fprintf(stderr, "  %s\n", err); return; }
  for (i = 0; i < sizeof entries / sizeof *entries; ++i) {
    const unsigned char *f = pe_symbol_coff(img, entries[i], PE_SYM_EXTERNAL, PE_COFF_IMAGE);
    /* push ebp; mov ebp, esp: each is the start of a function */
    CHECK(f && f[0] == 0x55 && f[1] == 0x8b && f[2] == 0xec);
  }
  CHECK(pe_symbol_coff(img, "_tva_kf_type_off_tbl", PE_SYM_STATIC, PE_COFF_IMAGE) != NULL);
  pe_unload(img);
#else
  SKIP("vsc.sys is a 32-bit image");
#endif
}

int main(void)
{
  if (access(probe_path(), R_OK)) {
    fprintf(stderr, "FAIL %s is not built (make peprobes)\n", probe);
    return 1;
  }
  RUN(imports_call_what_the_resolver_bound_them_to);
  RUN(the_resolver_is_told_the_library_and_the_function);
  RUN(an_import_nothing_binds_fails_the_load_and_names_it);
  RUN(the_entry_point_runs_unless_the_caller_says_not_to);
  RUN(a_table_binds_by_name_whatever_the_library);
  RUN(a_missing_file_is_named);
  RUN(an_external_symbol_is_found_by_its_long_name);
  RUN(an_external_symbol_is_found_by_a_name_short_enough_to_be_inline);
  RUN(a_file_static_is_found_as_a_static_and_only_as_one);
  RUN(a_name_is_matched_whole);
  RUN(vsc_sys_maps_and_its_entry_points_resolve);
  return test_summary();
}
