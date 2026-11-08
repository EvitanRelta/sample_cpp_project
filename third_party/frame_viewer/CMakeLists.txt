cmake_minimum_required(VERSION 3.16)
project(frame_viewer VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Generate build/compile_commands.json for clangd.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs)
find_package(Threads REQUIRED)

# ---- Library ----------------------------------------------------------------------------------
add_library(frame_viewer STATIC
    src/buffer_pool.cpp
    src/compositor.cpp
    src/key_utils.cpp
    src/ui_thread.cpp
    src/view_widget.cpp
    src/viewer.cpp
)
target_include_directories(frame_viewer
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
# The public header uses Qt value types (keys, mouse buttons) and OpenCV types.
# Qt Widgets is an implementation detail.
target_link_libraries(frame_viewer
    PUBLIC  Qt5::Core Qt5::Gui opencv_core Threads::Threads
    PRIVATE Qt5::Widgets opencv_imgproc
)
target_compile_options(frame_viewer PRIVATE -Wall -Wextra)

# A parent project that vendors this repo can skip these with
# set(FRAME_VIEWER_BUILD_DEMOS OFF) / set(FRAME_VIEWER_BUILD_TESTS OFF) before add_subdirectory().
option(FRAME_VIEWER_BUILD_DEMOS "Build the demos" ON)
option(FRAME_VIEWER_BUILD_TESTS "Build the tests" ON)
if(NOT FRAME_VIEWER_BUILD_DEMOS AND NOT FRAME_VIEWER_BUILD_TESTS)
    return()
endif()

# Put the executables in bin/ unless the user or a parent project already chose a directory.
# The variable is scoped to this directory, so a parent project is not affected.
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endif()

# ---- Demos ------------------------------------------------------------------------------------
if(FRAME_VIEWER_BUILD_DEMOS)
    foreach(demo demo_simple demo_keys demo_multi demo_mosaic demo_big_frame demo_roi demo_canvas)
        add_executable(${demo} demos/${demo}.cpp)
        target_link_libraries(${demo} PRIVATE frame_viewer opencv_imgproc)
        target_compile_options(${demo} PRIVATE -Wall -Wextra)
    endforeach()
    target_link_libraries(demo_keys PRIVATE opencv_imgcodecs)
    target_link_libraries(demo_canvas PRIVATE opencv_imgcodecs)
endif()

# ---- Tests ------------------------------------------------------------------------------------
# Needs a display (opens a window briefly). Run it directly: ./build/bin/test_show_shared
if(FRAME_VIEWER_BUILD_TESTS)
    add_executable(test_show_shared tests/test_show_shared.cpp)
    target_link_libraries(test_show_shared PRIVATE frame_viewer opencv_imgproc)
    target_compile_options(test_show_shared PRIVATE -Wall -Wextra)
endif()
