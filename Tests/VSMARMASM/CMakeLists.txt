cmake_minimum_required(VERSION 3.25) # Enable CMP0141
cmake_policy(SET CMP0184 NEW)
project(VSMARMASM C ASM_MARMASM)
add_executable(VSMARMASM main.c foo.asm)
target_compile_options(VSMARMASM PRIVATE
  "$<$<COMPILE_LANGUAGE:ASM_MARMASM>:SHELL:-predefine \"zero SETA 0\">"
  )
add_library(empty STATIC empty.asm)
