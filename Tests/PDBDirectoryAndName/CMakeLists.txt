cmake_minimum_required(VERSION 3.10)
project(PDBDirectoryAndName C)

# Make sure the proper compiler is in use.
if(NOT MSVC AND NOT CMAKE_C_COMPILER_ID STREQUAL "Intel" AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
  message(FATAL_ERROR "The PDBDirectoryAndName test works only with MSVC, Clang or Intel")
endif()

# Intel 11.1 does not support /Fd but Intel 14.0 does.
# TODO: Did a version in between these add it?
if((CMAKE_C_COMPILER_ID STREQUAL Intel AND
    CMAKE_C_COMPILER_VERSION VERSION_LESS 14.0) OR
   CMAKE_C_COMPILER_ID STREQUAL "IntelLLVM")
  set(NO_COMPILE_PDB 1)
endif()

set(my_targets "")

add_library(mylibA SHARED mylibA.c)
set_target_properties(mylibA PROPERTIES
    PDB_NAME "mylibA_Special"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mylibA_PDB/$<CONFIG>"
)
list(APPEND my_targets mylibA)

add_library(mylibB STATIC mylibB.c)
set_target_properties(mylibB PROPERTIES
    COMPILE_PDB_NAME "mylibB_Special"
    COMPILE_PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/mylibB_PDB"
)
if(NOT NO_COMPILE_PDB)
  list(APPEND my_targets mylibB)
endif()

add_library(mylibC SHARED mylibC.c)
set_target_properties(mylibC PROPERTIES
    PDB_NAME "mylibC_Special"
)
list(APPEND my_targets mylibC)

add_library(mylibD STATIC mylibD.c)
set_target_properties(mylibD PROPERTIES
    COMPILE_PDB_NAME "mylibD_Special"
)
if(NOT NO_COMPILE_PDB)
  list(APPEND my_targets mylibD)
endif()

add_executable(myexe myexe.c)
set_target_properties(myexe PROPERTIES
    PDB_NAME "myexe_Special"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/myexe_PDB"
)
list(APPEND my_targets myexe)

target_link_libraries(myexe mylibA mylibB mylibC mylibD)

add_executable(myexe2 myexe2.c)
set_target_properties(myexe2 PROPERTIES
    PDB_NAME "myexe2_Special"
)
list(APPEND my_targets myexe2)

target_link_libraries(myexe2 mylibA mylibD)


# Clang/C2 does not produce pdb files for static libraries
if (CMAKE_C_COMPILER_ID STREQUAL "Clang" AND
    CMAKE_C_SIMULATE_ID STREQUAL "MSVC")
  list(REMOVE_ITEM my_targets mylibB mylibD)
endif()


#-----------------------------------------------------------------------------
# Check that PDB files actually appear where expected.

set(pdbs "")
foreach(t ${my_targets})
  set(with_compile 0)
  get_property(pdb_name TARGET ${t} PROPERTY PDB_NAME)
  get_property(pdb_dir TARGET ${t} PROPERTY PDB_OUTPUT_DIRECTORY)
  if(NOT pdb_name)
    set(with_compile 1)
    get_property(pdb_name TARGET ${t} PROPERTY COMPILE_PDB_NAME)
  endif()
  if(NOT pdb_dir)
    get_property(pdb_dir TARGET ${t} PROPERTY COMPILE_PDB_OUTPUT_DIRECTORY)
  endif()
  if(NOT pdb_dir)
    if (NOT with_compile)
      set(pdb_dir ${CMAKE_CURRENT_BINARY_DIR})
    elseif (CMAKE_GENERATOR MATCHES "Ninja" OR
            CMAKE_GENERATOR MATCHES "Makefiles")
      set(pdb_dir ${CMAKE_CURRENT_BINARY_DIR}/CMakeFiles/${t}.dir)
    elseif (CMAKE_GENERATOR MATCHES "Visual Studio")
      set(pdb_dir ${CMAKE_CURRENT_BINARY_DIR}/${t}.dir)
    else ()
      set(pdb_dir ${CMAKE_CURRENT_BINARY_DIR}/${t}.dir)
    endif ()
  endif()
  if (pdb_dir MATCHES "\\$<.*>")
    # Skip per-configuration subdirectory if the value contained
    # a generator expression.
    list(APPEND pdbs ${pdb_dir}/${pdb_name}.pdb)
  else()
    list(APPEND pdbs ${pdb_dir}/${CMAKE_CFG_INTDIR}/${pdb_name}.pdb)
  endif()
endforeach()
add_custom_target(check_pdbs ALL VERBATIM
  COMMAND ${CMAKE_COMMAND} -Dconfig=$<CONFIGURATION> "-Dpdbs=${pdbs}"
                           -P ${CMAKE_CURRENT_SOURCE_DIR}/check_pdbs.cmake
  )
add_dependencies(check_pdbs ${my_targets})
