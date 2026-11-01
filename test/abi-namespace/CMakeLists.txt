# The ABI namespace (cmake/core-library.cmake, THL_ABI_NAMESPACE):
#
#   * abi_namespace_symbols_<library>: every C++ symbol a built library defines lies in its
#     namespace thl::<ns>, in std or in the third-party code it embeds, never at global scope
#     (check-abi-namespace.cmake: nm / dumpbin over the binary CMake produced);
#   * test_abi_namespace: a second core library, tanh_core_b in thl::test_b, linked into one
#     executable beside tanh_core, the way a host's tanh-lib and an embedder's private copy
#     are; two probe libraries compile the same probe.cpp, one against each, and the test
#     shows that the two loggers share no state;
#   * abi_namespace_sources: scripts/abi-namespace.py --check, every namespace opener of the
#     tree carries the namespace (a header no library instantiates is invisible to nm).
#
# Registered from the top-level CMakeLists.txt on ELF, Mach-O and PE when not
# cross-compiling.

include(${tanh_SOURCE_DIR}/cmake/core-library.cmake)
tanh_add_core_library(tanh_core_b
    SOURCE_DIR ${tanh_SOURCE_DIR}
    ABI_NAMESPACE test_b
    STATIC)
_tanh_apply_sanitizers(tanh_core_b)

# One translation unit per library: each probe library sees only its own core's headers.
add_library(abi_probe_main STATIC probe.cpp)
target_link_libraries(abi_probe_main PRIVATE tanh::Core)
target_compile_definitions(abi_probe_main PRIVATE ABI_PROBE_NAMESPACE=probe_main)

add_library(abi_probe_b STATIC probe.cpp)
target_link_libraries(abi_probe_b PRIVATE tanh_core_b)
target_compile_definitions(abi_probe_b PRIVATE ABI_PROBE_NAMESPACE=probe_b)

get_target_property(_main_namespace tanh_core TANH_ABI_NAMESPACE)
add_executable(test_abi_namespace test_AbiNamespace.cpp)
target_link_libraries(test_abi_namespace PRIVATE abi_probe_main abi_probe_b gtest_main gtest)
target_compile_definitions(test_abi_namespace PRIVATE
    ABI_EXPECTED_MAIN_NAMESPACE="${_main_namespace}"
    ABI_EXPECTED_B_NAMESPACE="test_b")
tanh_copy_runtime_dlls(test_abi_namespace)
gtest_discover_tests(test_abi_namespace)

# The symbols of every component and of the second core library.
if(TANH_BINARY_FORMAT STREQUAL "ELF")
    set(_format ELF)
elseif(TANH_BINARY_FORMAT STREQUAL "Mach-O")
    set(_format MACHO)
else()
    set(_format PE)
endif()
if(_format STREQUAL "PE")
    # dumpbin lives next to link.exe; it is not on PATH outside a developer shell.
    get_filename_component(_linker_dir "${CMAKE_LINKER}" DIRECTORY)
    find_program(TANH_DUMPBIN dumpbin HINTS "${_linker_dir}")
    set(_tool "${TANH_DUMPBIN}")
    set(_tool_arg "-DDUMPBIN=${TANH_DUMPBIN}")
else()
    if(CMAKE_NM)
        set(_tool "${CMAKE_NM}")
    else()
        find_program(TANH_NM NAMES nm llvm-nm)
        set(_tool "${TANH_NM}")
    endif()
    set(_tool_arg "-DNM=${_tool}")
endif()

if(_tool)
    # Where a component may define symbols besides thl: the standard library's
    # instantiations, the bundled header-only code (nlohmann::json in State, moodycamel's
    # readerwriterqueue in Modulation) and miniaudio, whose C functions AudioIO compiles as
    # C++ (src/audio-io/miniaudio_impl.cpp), so they are mangled at global scope. The
    # implementation's reserved namespaces (__gnu_cxx, __rtsan, ...) need no entry.
    set(_allow_scopes std nlohmann moodycamel)
    set(_allow_global_prefixes ma_)
    foreach(_library IN LISTS TANH_BUILT_COMPONENTS ITEMS tanh_core_b)
        if(_library STREQUAL "tanh_core_b")
            set(_namespace test_b)
        else()
            set(_namespace "${_main_namespace}")
        endif()
        add_test(NAME abi_namespace_symbols_${_library}
            COMMAND ${CMAKE_COMMAND}
                -DFORMAT=${_format}
                ${_tool_arg}
                "-DLIBRARY=$<TARGET_FILE:${_library}>"
                -DNAMESPACE=${_namespace}
                "-DALLOW_SCOPES=${_allow_scopes}"
                "-DALLOW_GLOBAL_PREFIXES=${_allow_global_prefixes}"
                -P ${CMAKE_CURRENT_SOURCE_DIR}/check-abi-namespace.cmake)
    endforeach()
else()
    message(WARNING "test/abi-namespace: no nm/dumpbin found, the symbol checks are not registered")
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME abi_namespace_sources
        COMMAND ${Python3_EXECUTABLE} ${tanh_SOURCE_DIR}/scripts/abi-namespace.py
                --check --root ${tanh_SOURCE_DIR})
else()
    message(STATUS "test/abi-namespace: no Python 3, abi_namespace_sources is not registered")
endif()
