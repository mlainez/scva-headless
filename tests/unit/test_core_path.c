/* SPDX-License-Identifier: CC0-1.0
 *
 * Where the tools and the plugins look for SCCore.dll.
 */
#include "check.h"
#include "core_path.h"
#include "scva_lists.h"

TEST(an_explicit_core_path_wins_over_the_environment)
{
  setenv("SCVA_DLL_DIR", "/somewhere/else", 1);
  CHECK_STR(scva_core_path("/my/own/SCCore.dll"), "/my/own/SCCore.dll");
  unsetenv("SCVA_DLL_DIR");
}

TEST(scva_dll_dir_names_the_directory_holding_the_core)
{
  setenv("SCVA_DLL_DIR", "/opt/scva", 1);
  CHECK_STR(scva_core_path(NULL), "/opt/scva/SCCore.dll");
  CHECK_STR(scva_core_path(""), "/opt/scva/SCCore.dll");
  unsetenv("SCVA_DLL_DIR");
}

TEST(without_either_the_tools_look_in_dll_beside_them)
{
  unsetenv("SCVA_DLL_DIR");
  CHECK_STR(scva_core_path(NULL), "dll/SCCore.dll");
  setenv("SCVA_DLL_DIR", "", 1);
  CHECK_STR(scva_core_path(NULL), "dll/SCCore.dll");
  unsetenv("SCVA_DLL_DIR");
}

TEST(a_plugin_uses_scva_dll_dir_only_when_a_core_is_there)
{
  const char *with = test_tmpdir();
  const char *without = test_tmpdir();
  char buf[512];

  test_write(with, "SCCore.dll", "MZ");
  setenv("SCVA_DLL_DIR", with, 1);
  CHECK_STR(scva_core_dir("/plugin/bundle", buf, sizeof buf), with);

  setenv("SCVA_DLL_DIR", without, 1);
  CHECK_STR(scva_core_dir("/plugin/bundle", buf, sizeof buf), "/plugin/bundle");
  unsetenv("SCVA_DLL_DIR");
}

TEST(a_plugin_without_scva_dll_dir_looks_beside_itself)
{
  char buf[512];
  unsetenv("SCVA_DLL_DIR");
  CHECK_STR(scva_core_dir("/plugin/bundle", buf, sizeof buf), "/plugin/bundle");
  CHECK_STR(scva_core_dir(NULL, buf, sizeof buf), ".");
}

TEST(the_engine_minimum_block_is_256_frames)
{
  /* Below 255 frames the core corrupts its own heap; every tool clamps to
     this value. */
  CHECK_INT(SCVA_MIN_BLOCK, 256);
}

int main(void)
{
  RUN(an_explicit_core_path_wins_over_the_environment);
  RUN(scva_dll_dir_names_the_directory_holding_the_core);
  RUN(without_either_the_tools_look_in_dll_beside_them);
  RUN(a_plugin_uses_scva_dll_dir_only_when_a_core_is_there);
  RUN(a_plugin_without_scva_dll_dir_looks_beside_itself);
  RUN(the_engine_minimum_block_is_256_frames);
  return test_summary();
}
