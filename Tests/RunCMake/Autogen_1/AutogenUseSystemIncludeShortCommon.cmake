set(CMAKE_AUTOGEN_INTERMEDIATE_DIR_STRATEGY SHORT CACHE STRING "" FORCE)

enable_language(CXX)

find_package(Qt${with_qt_version} REQUIRED COMPONENTS Core Widgets Gui)

add_library(dummy SHARED empty.cpp)
target_link_libraries(dummy Qt${with_qt_version}::Core
                            Qt${with_qt_version}::Widgets
                            Qt${with_qt_version}::Gui)
