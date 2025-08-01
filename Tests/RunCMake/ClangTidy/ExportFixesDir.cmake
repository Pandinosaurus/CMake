set(CMAKE_INTERMEDIATE_DIR_STRATEGY FULL CACHE STRING "" FORCE)

enable_language(C)
set(CMAKE_C_CLANG_TIDY "${PSEUDO_TIDY}" -some -args)
set(CMAKE_C_CLANG_TIDY_EXPORT_FIXES_DIR clang-tidy)
set(files ${CMAKE_CURRENT_SOURCE_DIR}/main.c ${CMAKE_CURRENT_SOURCE_DIR}/extra.c)
add_executable(main ${files})
add_subdirectory(export_fixes_subdir)
