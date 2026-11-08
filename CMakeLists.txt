cmake_minimum_required(VERSION 3.21)

project(sample_cpp_project LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Debug
        CACHE STRING "Build type" FORCE)
endif()

option(SAMPLE_CPP_PROJECT_BUILD_EXTRAS "Build extra executables" ON)

find_package(OpenCV 4.5 REQUIRED)
message(
    STATUS "Found OpenCV VERSION: ${OpenCV_VERSION} at ${OpenCV_INCLUDE_DIRS}")

set(FRAME_VIEWER_BUILD_DEMOS
    OFF
    CACHE BOOL "" FORCE)
set(FRAME_VIEWER_BUILD_TESTS
    OFF
    CACHE BOOL "" FORCE)
add_subdirectory(third_party/frame_viewer)

function(sample_cpp_project_configure_target target_name)
    target_compile_features("${target_name}" PRIVATE cxx_std_20)
    set_target_properties(
        "${target_name}"
        PROPERTIES CXX_EXTENSIONS OFF
                   RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
                   LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
                   ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
endfunction()

file(GLOB_RECURSE SAMPLE_CPP_PROJECT_LIBRARY_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(REMOVE_ITEM SAMPLE_CPP_PROJECT_LIBRARY_SOURCES
     "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(sample_cpp_project_lib STATIC ${SAMPLE_CPP_PROJECT_LIBRARY_SOURCES})
target_include_directories(sample_cpp_project_lib
                           PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(sample_cpp_project_lib SYSTEM
                           PUBLIC ${OpenCV_INCLUDE_DIRS})
target_link_libraries(sample_cpp_project_lib PUBLIC ${OpenCV_LIBS})
sample_cpp_project_configure_target(sample_cpp_project_lib)

add_executable(sample_cpp_project src/main.cpp)
target_include_directories(
    sample_cpp_project
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include")
target_link_libraries(sample_cpp_project PRIVATE sample_cpp_project_lib)
sample_cpp_project_configure_target(sample_cpp_project)

if(SAMPLE_CPP_PROJECT_BUILD_EXTRAS)
    add_subdirectory(extras)
endif()
