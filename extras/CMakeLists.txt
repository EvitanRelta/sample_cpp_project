# Recursively discover all .cpp extras so nested subdirectories work without
# CMake edits. Every extra source must define exactly one main().
file(GLOB_RECURSE SAMPLE_CPP_PROJECT_EXTRA_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

foreach(extra_source IN LISTS SAMPLE_CPP_PROJECT_EXTRA_SOURCES)
    file(RELATIVE_PATH extra_relative "${CMAKE_CURRENT_SOURCE_DIR}"
         "${extra_source}")
    string(REGEX REPLACE "[.]cpp$" "" extra_path_without_extension
                         "${extra_relative}")
    string(REPLACE "/" "_" extra_name "${extra_path_without_extension}")
    set(extra_target "sample_cpp_project_extra_${extra_name}")

    # Reject ambiguous paths such as a/b.cpp and a_b.cpp rather than silently
    # combining or replacing targets.
    if(TARGET "${extra_target}")
        get_target_property(existing_extra_source "${extra_target}" SOURCES)
        message(
            FATAL_ERROR
                "Flattened extra target collision: ${existing_extra_source} and ${extra_source} both map to ${extra_target}"
        )
    endif()

    get_filename_component(extra_output_name "${extra_relative}" NAME_WLE)
    get_filename_component(extra_output_directory "${extra_relative}" DIRECTORY)

    add_executable("${extra_target}" "${extra_source}")
    target_link_libraries("${extra_target}" PRIVATE sample_cpp_project_lib)
    sample_cpp_project_configure_target("${extra_target}")
    set_target_properties(
        "${extra_target}"
        PROPERTIES OUTPUT_NAME "${extra_output_name}"
                   RUNTIME_OUTPUT_DIRECTORY
                   "${CMAKE_BINARY_DIR}/extras/${extra_output_directory}")
endforeach()
