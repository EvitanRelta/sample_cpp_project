cmake_minimum_required(VERSION 3.21)

project(sample_cpp_project LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE
        Debug
        CACHE STRING "Build type")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release
                                                 RelWithDebInfo MinSizeRel)
endif()

option(SAMPLE_CPP_PROJECT_BUILD_TOOLS "Build tool executables" ON)

function(sample_cpp_project_configure_target target_name)
    target_compile_features("${target_name}" PRIVATE cxx_std_20)
    set_target_properties(
        "${target_name}"
        PROPERTIES CXX_EXTENSIONS OFF
                   MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
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
sample_cpp_project_configure_target(sample_cpp_project_lib)

add_executable(sample_cpp_project src/main.cpp)
target_include_directories(
    sample_cpp_project
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/include")
target_link_libraries(sample_cpp_project PRIVATE sample_cpp_project_lib)
sample_cpp_project_configure_target(sample_cpp_project)

if(SAMPLE_CPP_PROJECT_BUILD_TOOLS)
    add_subdirectory(tools)
endif()
