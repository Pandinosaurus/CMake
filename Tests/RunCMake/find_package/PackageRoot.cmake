cmake_policy(SET CMP0074 NEW)
list(INSERT CMAKE_MODULE_PATH 0 ${CMAKE_CURRENT_SOURCE_DIR}/PackageRoot)
set(PackageRoot_BASE ${CMAKE_CURRENT_SOURCE_DIR}/PackageRoot)

function(PrintPath label path)
  string(REPLACE "${PackageRoot_BASE}" "<base>" out "${path}")
  message("${label}${out}")
endfunction()

macro(CleanUpPackageRootTest)
  unset(Foo_ROOT)
  unset(ENV{Foo_ROOT})
  unset(FOO_TEST_FILE_FOO)
  unset(FOO_TEST_FILE_ZOT)
  unset(FOO_TEST_PATH_FOO)
  unset(FOO_TEST_PATH_ZOT)
  unset(FOO_TEST_PROG_FOO)
  unset(FOO_TEST_FILE_FOO CACHE)
  unset(FOO_TEST_FILE_ZOT CACHE)
  unset(FOO_TEST_PATH_FOO CACHE)
  unset(FOO_TEST_PATH_ZOT CACHE)
  unset(FOO_TEST_PROG_FOO CACHE)
endmacro()

macro(RunPackageRootTest)
  message("----------")
  PrintPath("Foo_ROOT      :" "${Foo_ROOT}")
  PrintPath("ENV{Foo_ROOT} :" "$ENV{Foo_ROOT}")
  message("")

  find_package(Foo)
  message("find_package(Foo)")
  PrintPath("FOO_TEST_FILE_FOO :" "${FOO_TEST_FILE_FOO}")
  PrintPath("FOO_TEST_FILE_ZOT :" "${FOO_TEST_FILE_ZOT}")
  PrintPath("FOO_TEST_PATH_FOO :" "${FOO_TEST_PATH_FOO}")
  PrintPath("FOO_TEST_PATH_ZOT :" "${FOO_TEST_PATH_ZOT}")
  PrintPath("FOO_TEST_PROG_FOO :" "${FOO_TEST_PROG_FOO}")
  CleanUpPackageRootTest()
  message("")
endmacro()

RunPackageRootTest()

set(Foo_ROOT      ${PackageRoot_BASE}/foo/cmake_root)
RunPackageRootTest()

set(ENV{Foo_ROOT} ${PackageRoot_BASE}/foo/env_root)
RunPackageRootTest()

set(Foo_ROOT      ${PackageRoot_BASE}/foo/cmake_root)
set(ENV{Foo_ROOT} ${PackageRoot_BASE}/foo/env_root)
RunPackageRootTest()
