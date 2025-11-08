macro(tanh_make_absolute path)
    if(NOT IS_ABSOLUTE "${${path}}")
        get_filename_component(${path} "${${path}}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
    endif()

    string(REGEX REPLACE "\\\\" "/" ${path} "${${path}}")
endmacro()

function(tanh_add_binary_data target)
    set(one_value_args NAMESPACE HEADER_NAME)
    set(multi_value_args SOURCES)
    cmake_parse_arguments(TANH_ARG "" "${one_value_args}" "${multi_value_args}" ${ARGN})

    list(LENGTH TANH_ARG_SOURCES num_binary_files)

    if(${num_binary_files} LESS 1)
        message(FATAL_ERROR "tanh_add_binary_data must be passed at least one file to encode")
    endif()

    add_library(${target} STATIC)

    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE TRUE)

    set(tanh_binary_data_folder "${CMAKE_CURRENT_BINARY_DIR}/tanh_binarydata_${target}/TanhLibraryCode")

    file(MAKE_DIRECTORY ${tanh_binary_data_folder})

    if(NOT TANH_ARG_NAMESPACE)
        set(TANH_ARG_NAMESPACE BinaryData)
    endif()

    if(NOT TANH_ARG_HEADER_NAME)
        set(TANH_ARG_HEADER_NAME BinaryData.h)
    endif()

    set(binary_data_header "${tanh_binary_data_folder}/${TANH_ARG_HEADER_NAME}")
    set(binary_data_sources "")

    set(header_content "// Auto-generated binary data file\n")
    set(header_content "${header_content}#pragma once\n\n")
    set(header_content "${header_content}namespace ${TANH_ARG_NAMESPACE}\n{\n")

    set(file_index 1)
    foreach(source_file IN LISTS TANH_ARG_SOURCES)
        tanh_make_absolute(source_file)

        get_filename_component(file_name "${source_file}" NAME)
        string(MAKE_C_IDENTIFIER "${file_name}" var_name)

        set(header_content "${header_content}    extern const char*   ${var_name};\n")
        set(header_content "${header_content}    const int            ${var_name}Size = ")

        file(SIZE "${source_file}" file_size)
        set(header_content "${header_content}${file_size};\n\n")

        set(cpp_file "${tanh_binary_data_folder}/BinaryData${file_index}.cpp")
        list(APPEND binary_data_sources "${cpp_file}")

        file(READ "${source_file}" file_data HEX)

        set(cpp_content "// Auto-generated binary data file\n")
        set(cpp_content "${cpp_content}#include \"${TANH_ARG_HEADER_NAME}\"\n\n")
        set(cpp_content "${cpp_content}namespace ${TANH_ARG_NAMESPACE}\n{\n")

        string(LENGTH "${file_data}" data_length)
        set(cpp_content "${cpp_content}static const unsigned char ${var_name}_data[] = {\n")

        if(data_length GREATER 0)
            set(byte_count 0)
            set(line_content "    ")
            math(EXPR hex_pairs "${data_length} / 2")

            foreach(i RANGE 0 ${hex_pairs})
                math(EXPR pos "${i} * 2")
                string(SUBSTRING "${file_data}" ${pos} 2 byte_hex)
                if(NOT "${byte_hex}" STREQUAL "")
                    set(line_content "${line_content}0x${byte_hex},")
                    math(EXPR byte_count "${byte_count} + 1")
                    math(EXPR mod_result "${byte_count} % 16")
                    if(mod_result EQUAL 0)
                        set(cpp_content "${cpp_content}${line_content}\n")
                        set(line_content "    ")
                    else()
                        set(line_content "${line_content} ")
                    endif()
                endif()
            endforeach()

            if(NOT "${line_content}" STREQUAL "    ")
                set(cpp_content "${cpp_content}${line_content}\n")
            endif()
        endif()

        set(cpp_content "${cpp_content}};\n\n")
        set(cpp_content "${cpp_content}const char* ${var_name} = reinterpret_cast<const char*>(${var_name}_data);\n\n")
        set(cpp_content "${cpp_content}}\n")

        file(WRITE "${cpp_file}" "${cpp_content}")

        math(EXPR file_index "${file_index} + 1")
    endforeach()

    set(header_content "${header_content}}\n")
    file(WRITE "${binary_data_header}" "${header_content}")

    target_sources(${target} PRIVATE ${binary_data_sources})
    target_include_directories(${target} PUBLIC ${tanh_binary_data_folder})
    target_compile_features(${target} PRIVATE cxx_std_20)

    # This fixes an issue where Xcode is unable to find binary data during archive.
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${target} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "./")
    endif()
endfunction()