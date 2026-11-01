# ==============================================================================
# The core component (tanh::Core) as a library of the calling build.
#
#   include(<tanh-lib>/cmake/core-library.cmake)
#   tanh_add_core_library(<target> SOURCE_DIR <tanh-lib> ABI_NAMESPACE <ns>
#                         [STATIC | SHARED] [LOG_COMPILED_MAX_LEVEL <AUTO | 1..4>]
#                         [WITH_JOURNALD] [ALIAS <name>])
#
# Defines <target> from the core sources under SOURCE_DIR, with everything the core needs:
# the include directories, the PUBLIC definitions (TANH_VERSION, THL_LOG_COMPILED_MAX_LEVEL,
# the THL_PLATFORM_* and build-type macros), Android's log library, the optional journald
# sink, the symbol policy (cmake/tanh/symbol-policy.cmake) and the version properties.
# tanh-lib's own tanh_core is defined through it.
#
# ABI_NAMESPACE names the inline namespace every tanh-lib declaration lives in:
# thl::<ns>, spelled THL_ABI_NAMESPACE in the sources. configure_file writes it from
# cmake/AbiNamespace.h.in into <build>/generated/<target>/include/tanh/core/AbiNamespace.h,
# which is in the target's PUBLIC include directories, so every component built on the core
# and every consumer compiles against the library's value. Two core libraries with different
# namespaces share no symbol and no state, in one build and in one process, whatever the
# link mode: tanh-lib's own tanh_core (v<major>_<minor>) and an embedder's private copy, e.g.
#
#   FetchContent_Declare(anira_tanh_lib GIT_REPOSITORY ... GIT_TAG ... SOURCE_SUBDIR none)
#   FetchContent_MakeAvailable(anira_tanh_lib)
#   include(${anira_tanh_lib_SOURCE_DIR}/cmake/core-library.cmake)
#   tanh_add_core_library(anira_tanh_core SOURCE_DIR ${anira_tanh_lib_SOURCE_DIR}
#                         ABI_NAMESPACE anira STATIC LOG_COMPILED_MAX_LEVEL 4)
#
# STATIC / SHARED: the library type; neither follows BUILD_SHARED_LIBS.
# LOG_COMPILED_MAX_LEVEL: the most verbose log level compiled into the logger's call sites,
# 1 Error .. 4 Debug; AUTO (the default) is 1 in Release builds and 4 otherwise.
# WITH_JOURNALD: Linux only, routes the platform sink through sd_journal_send() (libsystemd).
# ALIAS: an ALIAS target for the library, e.g. tanh::Core.
#
# Target properties set for the caller: TANH_ABI_NAMESPACE (the namespace) and
# TANH_ABI_NAMESPACE_HEADER (the generated header's path, for an install rule).
# Sanitizers and install rules are the caller's.
#
# No include_guard, on purpose: every caller includes the file of the tree it builds right
# before the call, so two tanh-lib trees in one build (a host's and an embedder's) each
# define the core with their own function.
# ==============================================================================

include(GNUInstallDirs)
include("${CMAKE_CURRENT_LIST_DIR}/tanh/platform.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tanh/git-version.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tanh/symbol-policy.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tanh/apple.cmake")

function(tanh_add_core_library target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "STATIC;SHARED;WITH_JOURNALD"
        "SOURCE_DIR;ABI_NAMESPACE;LOG_COMPILED_MAX_LEVEL;ALIAS" "")
    if(arg_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "tanh_add_core_library: unexpected arguments: ${arg_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT arg_SOURCE_DIR OR NOT EXISTS "${arg_SOURCE_DIR}/src/core.cpp")
        message(FATAL_ERROR "tanh_add_core_library(${target}): SOURCE_DIR must be a tanh-lib "
                            "source tree (got '${arg_SOURCE_DIR}')")
    endif()
    if(NOT arg_ABI_NAMESPACE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR "tanh_add_core_library(${target}): ABI_NAMESPACE must be a C++ "
                            "identifier (got '${arg_ABI_NAMESPACE}')")
    endif()
    if(arg_STATIC AND arg_SHARED)
        message(FATAL_ERROR "tanh_add_core_library(${target}): give STATIC or SHARED, not both")
    endif()
    if(NOT DEFINED arg_LOG_COMPILED_MAX_LEVEL)
        set(arg_LOG_COMPILED_MAX_LEVEL AUTO)
    endif()
    if(NOT arg_LOG_COMPILED_MAX_LEVEL MATCHES "^(AUTO|[1-4])$")
        message(FATAL_ERROR "tanh_add_core_library(${target}): LOG_COMPILED_MAX_LEVEL must be AUTO "
                            "or 1..4, got '${arg_LOG_COMPILED_MAX_LEVEL}'")
    endif()

    set(_type "")
    if(arg_STATIC)
        set(_type STATIC)
    elseif(arg_SHARED)
        set(_type SHARED)
    endif()

    set(_src "${arg_SOURCE_DIR}")
    add_library(${target} ${_type}
        ${_src}/src/core.cpp
        ${_src}/src/core/Dispatcher.cpp
        ${_src}/src/core/Logger.cpp
        ${_src}/src/core/Thread.cpp
        ${_src}/src/core/RCU.cpp
    )
    if(arg_ALIAS)
        add_library(${arg_ALIAS} ALIAS ${target})
    endif()

    # The version of the sources compiled here, not of the calling project.
    tanh_git_version("${_src}")
    string(REGEX MATCH "^[0-9]+" _major "${TANH_VERSION_SHORT}")
    set_target_properties(${target} PROPERTIES
        VERSION ${TANH_VERSION_SHORT}
        SOVERSION ${_major})

    # tanh/core/AbiNamespace.h: THL_ABI_NAMESPACE for this library and its consumers.
    set(THL_ABI_NAMESPACE "${arg_ABI_NAMESPACE}")
    set(_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/${target}/include")
    configure_file("${_src}/cmake/AbiNamespace.h.in"
        "${_generated}/tanh/core/AbiNamespace.h" @ONLY)
    set_target_properties(${target} PROPERTIES
        TANH_ABI_NAMESPACE "${arg_ABI_NAMESPACE}"
        TANH_ABI_NAMESPACE_HEADER "${_generated}/tanh/core/AbiNamespace.h")
    message(STATUS "tanh: ${target}: ABI namespace thl::${arg_ABI_NAMESPACE}")

    target_include_directories(${target}
        PUBLIC
        $<BUILD_INTERFACE:${_src}/include>
        $<BUILD_INTERFACE:${_generated}>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    )

    target_compile_definitions(${target}
        PUBLIC
        # Version number
        -DTANH_VERSION="${TANH_VERSION_FULL}"
    )
    if(arg_LOG_COMPILED_MAX_LEVEL STREQUAL "AUTO")
        target_compile_definitions(${target}
            PUBLIC
            $<$<CONFIG:Release>:THL_LOG_COMPILED_MAX_LEVEL=1>
            $<$<NOT:$<CONFIG:Release>>:THL_LOG_COMPILED_MAX_LEVEL=4>
        )
    else()
        target_compile_definitions(${target}
            PUBLIC THL_LOG_COMPILED_MAX_LEVEL=${arg_LOG_COMPILED_MAX_LEVEL}
        )
    endif()

    # Platform compile definitions (THL_PLATFORM_<OS>=1, THL_PLATFORM_IOS_<DEVICE|SIMULATOR>=1)
    target_compile_definitions(${target} PUBLIC ${TANH_PLATFORM_COMPILE_DEFINITIONS})

    # Build type definitions
    target_compile_definitions(${target}
        PUBLIC
        $<$<CONFIG:Debug>:THL_DEBUG=1>
        $<$<CONFIG:Release>:THL_RELEASE=1>
        $<$<CONFIG:RelWithDebInfo>:THL_RELWITHDEBINFO=1>
        $<$<CONFIG:MinSizeRel>:THL_MINSIZEREL=1>
    )

    if(TANH_OPERATING_SYSTEM STREQUAL "Android")
        target_link_libraries(${target} PRIVATE log)
    elseif(TANH_OPERATING_SYSTEM STREQUAL "Linux" AND arg_WITH_JOURNALD)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
        target_link_libraries(${target} PRIVATE PkgConfig::SYSTEMD)
        target_compile_definitions(${target} PRIVATE THL_WITH_JOURNALD=1)
    endif()

    # Hidden visibility, TANH_API through TANH_BUILDING / TANH_STATIC, and a shared library's
    # export table pinned to namespace thl (the top-level CMakeLists.txt explains the policy).
    tanh_apply_symbol_policy(${target} EXPORT_PREFIX TANH)
    tanh_set_export_allowlist(${target} NAMESPACE thl)

    # Only apps need a signature on iOS.
    tanh_ios_disable_code_signing(${target})
endfunction()
