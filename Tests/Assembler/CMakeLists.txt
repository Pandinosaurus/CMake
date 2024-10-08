cmake_minimum_required (VERSION 3.10)
if(POLICY CMP0129)
  cmake_policy(SET CMP0129 NEW)
endif()
project(Assembler C)
message("CTEST_FULL_OUTPUT ")
set(CMAKE_VERBOSE_MAKEFILE 1)

set(SRCS)

# (at least) the following toolchains can process assembler files directly
# and also generate assembler files from C:
if("${CMAKE_GENERATOR}" MATCHES "Makefile|Xcode|Ninja" AND
    NOT CMAKE_OSX_ARCHITECTURES MATCHES ";")
  if((CMAKE_C_COMPILER_ID MATCHES "^(GNU|LCC|Clang|AppleClang|HP|SunPro|XL|OrangeC)$") OR (CMAKE_C_COMPILER_ID MATCHES "Intel"  AND  UNIX)
     AND NOT (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND "x${CMAKE_C_COMPILER_FRONTEND_VARIANT}" STREQUAL "xMSVC"))
    set(C_FLAGS "${CMAKE_C_FLAGS}")
    separate_arguments(C_FLAGS)
    if(CMAKE_OSX_SYSROOT AND CMAKE_C_SYSROOT_FLAG AND NOT ";${C_FLAGS};" MATCHES ";${CMAKE_C_SYSROOT_FLAG};")
      list(APPEND C_FLAGS ${CMAKE_C_SYSROOT_FLAG} ${CMAKE_OSX_SYSROOT})
    endif()
    if(CMAKE_OSX_ARCHITECTURES)
      list(APPEND C_FLAGS -arch ${CMAKE_OSX_ARCHITECTURES})
    elseif("${CMAKE_SYSTEM_NAME};${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "Darwin;arm64")
      list(APPEND C_FLAGS -arch arm64)
    endif()
    if(CMAKE_C_COMPILER_ID MATCHES "Clang")
      # Just in case the user is passing -flto, we need to pass -fno-lto to
      # clang when generating the assembly file, or else clang will generate
      # LLVM IR instead of assembly.
      list(APPEND C_FLAGS -fno-lto)
    endif()
    # Clang on OS X, and perhaps other compilers, do not support -g
    # for both generating and assembling, so drop it from generating.
    list(REMOVE_ITEM C_FLAGS -g)
    set(SRCS main.s)
    add_custom_command(
      OUTPUT main.s
      COMMAND ${CMAKE_C_COMPILER} ${C_FLAGS}
        "$<$<CONFIG:Debug>:${CMAKE_C_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDebugDLL}>"
        "$<$<NOT:$<CONFIG:Debug>>:${CMAKE_C_COMPILE_OPTIONS_MSVC_RUNTIME_LIBRARY_MultiThreadedDLL}>"
        -S ${CMAKE_CURRENT_SOURCE_DIR}/main.c -o main.s
      COMMAND_EXPAND_LISTS
      DEPENDS main.c
      VERBATIM
      )
  endif()
endif()


if(SRCS)
  set(CMAKE_ASM_FLAGS "${CMAKE_C_FLAGS}")
  enable_language(ASM OPTIONAL)
else()
  message(STATUS "No assembler enabled, using C")
  set(SRCS main.c)
endif()

add_executable(HelloAsm ${SRCS})
