# ==============================================================================
# Install the library
# ==============================================================================
#
# Installs the built components, their public headers and a CMake package
# (tanhConfig.cmake + tanhTargets.cmake) so consumers can do
#
#     find_package(tanh REQUIRED COMPONENTS Core)
#     target_link_libraries(app PRIVATE tanh::Core)
#
# Works both for top-level builds and when tanh-lib is embedded via
# FetchContent/add_subdirectory: in the latter case the components join the
# parent's install prefix, and the parent's own exported targets may reference
# tanh::<Component> (its Config must then find_dependency(tanh)).

# for CMAKE_INSTALL_INCLUDEDIR and others definition
include(GNUInstallDirs)

if(NOT TANH_BUILT_COMPONENTS)
    return()
endif()

# --- Public headers ------------------------------------------------------------

# Shared by every component (Logger.h pulls in the sanitizer macros).
install(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/include/tanh/tanh.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tanh
    COMPONENT dev
)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/tanh/utils
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tanh
    COMPONENT dev
)

# Component -> (header directory, umbrella header) under include/tanh/. The
# resonator headers live inside dsp/ and are installed by the DSP entry (the
# Resonator component is only ever built together with DSP).
set(_tanh_header_dirs
    "${PROJECT_NAME}_core=core"
    "${PROJECT_NAME}_state=state"
    "${PROJECT_NAME}_dsp=dsp"
    "${PROJECT_NAME}_modulation=modulation"
    "${PROJECT_NAME}_audio_io=audio-io")
set(_tanh_umbrella_headers
    "${PROJECT_NAME}_core=core.h"
    "${PROJECT_NAME}_state=state.h"
    "${PROJECT_NAME}_dsp=dsp.h"
    "${PROJECT_NAME}_audio_io=audio_io.h")

foreach(target IN LISTS TANH_BUILT_COMPONENTS)
    foreach(_entry IN LISTS _tanh_header_dirs)
        string(REPLACE "=" ";" _entry "${_entry}")
        list(GET _entry 0 _entry_target)
        list(GET _entry 1 _entry_dir)
        if(_entry_target STREQUAL target)
            install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/tanh/${_entry_dir}
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tanh
                COMPONENT dev
            )
        endif()
    endforeach()
    foreach(_entry IN LISTS _tanh_umbrella_headers)
        string(REPLACE "=" ";" _entry "${_entry}")
        list(GET _entry 0 _entry_target)
        list(GET _entry 1 _entry_file)
        if(_entry_target STREQUAL target)
            install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/include/tanh/${_entry_file}
                DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/tanh
                COMPONENT dev
            )
        endif()
    endforeach()
endforeach()

# --- Targets -------------------------------------------------------------------

set(TARGETS_TO_EXPORT ${TANH_BUILT_COMPONENTS})

# nlohmann_json is a PUBLIC dependency of State; export it with the set so the
# installed tanh::State target resolves (its own config is also installed when
# JSON_Install is ON, which Config.cmake.in re-finds via find_dependency).
if(TARGET ${PROJECT_NAME}_state AND TARGET nlohmann_json)
    list(APPEND TARGETS_TO_EXPORT nlohmann_json)
endif()

install(TARGETS ${TARGETS_TO_EXPORT}
    EXPORT "tanhTargets"
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT runtime
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT runtime NAMELINK_COMPONENT dev
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev
)

# Only a top-level build may pick the install prefix; as a sub-project the
# components install wherever the parent does.
if(PROJECT_IS_TOP_LEVEL AND DEFINED CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    message(STATUS "CMAKE_INSTALL_PREFIX will be set to ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}")
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}" CACHE PATH "Where the library will be installed to" FORCE)
endif()

# At install the rpath is cleared by default, so the installed shared
# libraries need it re-set to find their sibling components. $ORIGIN (Linux)
# and @loader_path (macOS) resolve to the directory of the loading library.
if(TANH_OPERATING_SYSTEM STREQUAL "Linux")
    set_target_properties(${TANH_BUILT_COMPONENTS} PROPERTIES INSTALL_RPATH "$ORIGIN")
elseif(TANH_OPERATING_SYSTEM STREQUAL "macOS")
    set_target_properties(${TANH_BUILT_COMPONENTS} PROPERTIES INSTALL_RPATH "@loader_path")
endif()

# ==============================================================================
# Generate cmake config files
# ==============================================================================

include(CMakePackageConfigHelpers)

# Public component names (the EXPORT_NAME of each built target) for the Config.
set(TANH_EXPORTED_COMPONENTS "")
foreach(target IN LISTS TANH_BUILT_COMPONENTS)
    get_target_property(_export_name ${target} EXPORT_NAME)
    list(APPEND TANH_EXPORTED_COMPONENTS ${_export_name})
endforeach()

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    COMPONENT dev
)

install(EXPORT "tanhTargets"
    FILE tanhTargets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    COMPONENT dev
)
