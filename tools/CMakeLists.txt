# Recursively discover all .cpp tools so nested subdirectories work without
# CMake edits. Every tool source must define exactly one main().
file(GLOB_RECURSE SAMPLE_CPP_PROJECT_TOOL_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(tool_source IN LISTS SAMPLE_CPP_PROJECT_TOOL_SOURCES)
    file(RELATIVE_PATH tool_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${tool_source}")
    string(REGEX REPLACE "[.]cpp$" "" tool_path_without_extension
                         "${tool_relative}")
    string(REPLACE "/" "_" tool_name "${tool_path_without_extension}")
    set(tool_target "sample_cpp_project_tool_${tool_name}")

    # Reject ambiguous paths such as a/b.cpp and a_b.cpp rather than silently
    # combining or replacing targets.
    if(TARGET "${tool_target}")
        get_target_property(existing_tool_source "${tool_target}" SOURCES)
        message(
            FATAL_ERROR
                "Flattened tool target collision: ${existing_tool_source} and ${tool_source} both map to ${tool_target}"
        )
    endif()

    get_filename_component(tool_output_name "${tool_relative}" NAME_WLE)
    get_filename_component(tool_output_directory "${tool_relative}" DIRECTORY)

    add_executable("${tool_target}" "${tool_source}")
    target_link_libraries("${tool_target}" PRIVATE sample_cpp_project_lib)
    sample_cpp_project_configure_target("${tool_target}")
    set_target_properties(
        "${tool_target}"
        PROPERTIES OUTPUT_NAME "${tool_output_name}"
                   RUNTIME_OUTPUT_DIRECTORY
                   "${CMAKE_BINARY_DIR}/tools/${tool_output_directory}")
endforeach()
