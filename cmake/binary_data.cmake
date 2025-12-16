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

    set(registry_variable_names "")
    set(registry_file_names "")

    set(header_content "// Auto-generated binary data file\n")
    set(header_content "${header_content}#pragma once\n\n")
    set(header_content "${header_content}namespace ${TANH_ARG_NAMESPACE}\n{\n")

    set(header_content "${header_content}    // Retrieves binary data by original filename (returns nullptr if not found)\n")
    set(header_content "${header_content}    const char* getNamedResource(const char* fileName, int& sizeInBytes);\n\n")

    set(header_content "${header_content}    extern const char* namedResourceList[];\n")
    set(header_content "${header_content}    extern const char* originalFilenames[];\n")
    set(header_content "${header_content}    extern const int   resourceCount;\n\n")

    set(file_index 1)
    foreach(source_file IN LISTS TANH_ARG_SOURCES)
        message(STATUS "Encoding binary data file: ${source_file}")
        tanh_make_absolute(source_file)

        get_filename_component(file_name "${source_file}" NAME)
        string(MAKE_C_IDENTIFIER "${file_name}" var_name)

        list(APPEND registry_variable_names "${var_name}")
        list(APPEND registry_file_names "${file_name}")

        set(header_content "${header_content}    extern const char* ${var_name};\n")
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
            string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1, " file_data "${file_data}")
            string(REGEX REPLACE "((0x[0-9a-f][0-9a-f], ){16})" "\\1\n    " file_data "${file_data}")
            set(cpp_content "${cpp_content}    ${file_data}")
        endif()

        set(cpp_content "${cpp_content}};\n\n")
        set(cpp_content "${cpp_content}const char* ${var_name} = reinterpret_cast<const char*>(${var_name}_data);\n\n")
        set(cpp_content "${cpp_content}}\n")

        file(WRITE "${cpp_file}" "${cpp_content}")

        math(EXPR file_index "${file_index} + 1")
    endforeach()

    set(header_content "${header_content}}\n")
    file(WRITE "${binary_data_header}" "${header_content}")

    set(registry_cpp "${tanh_binary_data_folder}/BinaryDataRegistry.cpp")
    list(APPEND binary_data_sources "${registry_cpp}")

    set(reg_content "// Auto-generated registry file\n")
    set(reg_content "${reg_content}#include \"${TANH_ARG_HEADER_NAME}\"\n")
    set(reg_content "${reg_content}#include <cstring>\n\n")
    set(reg_content "${reg_content}namespace ${TANH_ARG_NAMESPACE}\n{\n")

    set(reg_content "${reg_content}    // Internal arrays for lookup\n")
    set(reg_content "${reg_content}    static const char* const dataPointers[] = {\n")
    foreach(var_name IN LISTS registry_variable_names)
        set(reg_content "${reg_content}        ${var_name},\n")
    endforeach()
    set(reg_content "${reg_content}        nullptr\n    };\n\n")

    set(reg_content "${reg_content}    static const int dataSizes[] = {\n")
    foreach(var_name IN LISTS registry_variable_names)
        set(reg_content "${reg_content}        ${var_name}Size,\n")
    endforeach()
    set(reg_content "${reg_content}        0\n    };\n\n")

    set(reg_content "${reg_content}    // Public Arrays\n")
    set(reg_content "${reg_content}    const char* namedResourceList[] = {\n")
    foreach(var_name IN LISTS registry_variable_names)
        set(reg_content "${reg_content}        \"${var_name}\",\n")
    endforeach()
    set(reg_content "${reg_content}        nullptr\n    };\n\n")

    set(reg_content "${reg_content}    const char* originalFilenames[] = {\n")
    foreach(file_name IN LISTS registry_file_names)
        set(reg_content "${reg_content}        \"${file_name}\",\n")
    endforeach()
    set(reg_content "${reg_content}        nullptr\n    };\n\n")

    list(LENGTH registry_variable_names num_resources)
    set(reg_content "${reg_content}    const int resourceCount = ${num_resources};\n\n")

    set(reg_content "${reg_content}    const char* getNamedResource(const char* fileName, int& sizeInBytes)\n    {\n")
    set(reg_content "${reg_content}        for(int i = 0; i < resourceCount; ++i)\n")
    set(reg_content "${reg_content}        {\n")
    set(reg_content "${reg_content}            if(std::strcmp(originalFilenames[i], fileName) == 0)\n")
    set(reg_content "${reg_content}            {\n")
    set(reg_content "${reg_content}                sizeInBytes = dataSizes[i];\n")
    set(reg_content "${reg_content}                return dataPointers[i];\n")
    set(reg_content "${reg_content}            }\n")
    set(reg_content "${reg_content}        }\n")
    set(reg_content "${reg_content}        sizeInBytes = 0;\n")
    set(reg_content "${reg_content}        return nullptr;\n")
    set(reg_content "${reg_content}    }\n")

    set(reg_content "${reg_content}}\n")
    file(WRITE "${registry_cpp}" "${reg_content}")

    target_sources(${target} PRIVATE ${binary_data_sources})
    target_include_directories(${target} PUBLIC ${tanh_binary_data_folder})
    target_compile_features(${target} PRIVATE cxx_std_20)

    # This fixes an issue where Xcode is unable to find binary data during archive.
    if(CMAKE_GENERATOR STREQUAL "Xcode")
        set_target_properties(${target} PROPERTIES ARCHIVE_OUTPUT_DIRECTORY "./")
    endif()
endfunction()