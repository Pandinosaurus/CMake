cmake_minimum_required(VERSION 3.10)
project(TestFindFreetype C)
include(CTest)

find_package(Freetype REQUIRED)

add_definitions(-DCMAKE_EXPECTED_FREETYPE_VERSION="${Freetype_VERSION}")

add_executable(testfreetype_tgt main.c)
target_link_libraries(testfreetype_tgt Freetype::Freetype)
add_test(NAME testfreetype_tgt COMMAND testfreetype_tgt)

add_executable(testfreetype_var main.c)
target_include_directories(testfreetype_var PRIVATE ${FREETYPE_INCLUDE_DIRS})
target_link_libraries(testfreetype_var PRIVATE ${FREETYPE_LIBRARIES})
add_test(NAME testfreetype_var COMMAND testfreetype_var)
