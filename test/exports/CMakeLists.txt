# The symbol-export invariant of every component (cmake/tanh/check-exports.cmake):
#
#   * a shared component exports namespace thl and nothing else — no std::, no
#     miniaudio (ma_*), no nlohmann::json, no moodycamel;
#   * tanh_exports_module, a plugin-shaped shared object embedding every built
#     component, exports its entry point only: in a static build nothing of thl:: may
#     leak from it, in either build nothing of the bundled third-party code;
#   * on ELF neither defines an STB_GNU_UNIQUE symbol (the module must be dlclose-able).
#
# Registered from the top-level CMakeLists.txt on ELF, Mach-O and PE when not
# cross-compiling (nm / dumpbin inspect the binaries CMake produced).
include(${CMAKE_SOURCE_DIR}/cmake/tanh/check-exports.cmake)

add_library(tanh_exports_module MODULE module.cpp)
target_link_libraries(tanh_exports_module PRIVATE ${TANH_BUILT_COMPONENTS})
tanh_apply_symbol_policy(tanh_exports_module)
tanh_set_export_allowlist(tanh_exports_module SYMBOL tanh_exports_entry)

foreach(_component IN LISTS TANH_BUILT_COMPONENTS)
    tanh_add_export_check(NAME exports_${_component}
        LIBRARY ${_component}
        MODULE tanh_exports_module
        NAMESPACES thl
        FORBID_NAMESPACES nlohmann moodycamel
        FORBID_PREFIXES ma_)
endforeach()
