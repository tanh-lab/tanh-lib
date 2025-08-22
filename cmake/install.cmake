# ==============================================================================
# Install the library
# ==============================================================================

# for CMAKE_INSTALL_INCLUDEDIR and others definition
include(GNUInstallDirs)

# Install headers
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/tanh
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    COMPONENT dev
)

# Install libraries and create export set
set(TARGETS_TO_INSTALL)
if(TARGET ${PROJECT_NAME}_core)
    list(APPEND TARGETS_TO_INSTALL ${PROJECT_NAME}_core)
endif()
if(TARGET ${PROJECT_NAME}_state)
    list(APPEND TARGETS_TO_INSTALL ${PROJECT_NAME}_state)
endif()
if(TARGET ${PROJECT_NAME}_dsp)
    list(APPEND TARGETS_TO_INSTALL ${PROJECT_NAME}_dsp)
endif()
if(TARGET ${PROJECT_NAME}_advanced)
    list(APPEND TARGETS_TO_INSTALL ${PROJECT_NAME}_advanced)
endif()
if(TARGET ${PROJECT_NAME}_processing)
    list(APPEND TARGETS_TO_INSTALL ${PROJECT_NAME}_processing)
endif()

if(TARGETS_TO_INSTALL)
    install(TARGETS ${TARGETS_TO_INSTALL} nlohmann_json
        EXPORT "tanhTargets"
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

# Note: We don't install nlohmann_json as it's an external dependency
# that consumers should find themselves via find_dependency(nlohmann_json)

# define the directory where the library will be installed CMAKE_INSTALL_PREFIX
if(DEFINED CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    message( STATUS "CMAKE_INSTALL_PREFIX will be set to ${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}" )
    set(CMAKE_INSTALL_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/${PROJECT_NAME}-${PROJECT_VERSION}" CACHE PATH "Where the library will be installed to" FORCE)
else()
    message(STATUS "CMAKE_INSTALL_PREFIX was already set to ${CMAKE_INSTALL_PREFIX}")
endif()

# at install the rpath is cleared by default so we have to set it again for the installed shared library to find the other libraries
# in this case we set the rpath to the directories where the other libraries are installed
# $ORIGIN in Linux is a special token that gets replaced by the directory of the library at runtime from that point we could navigate to the other libraries
# The same token for macOS is @loader_path
if(TANH_OPERATING_SYSTEM STREQUAL "Linux")
    foreach(target ${TARGETS_TO_INSTALL})
        set_target_properties(${target}
            PROPERTIES
                INSTALL_RPATH "$ORIGIN"
        )
    endforeach()
elseif(TANH_OPERATING_SYSTEM STREQUAL "macOS")
    foreach(target ${TARGETS_TO_INSTALL})
        set_target_properties(${target}
            PROPERTIES
                INSTALL_RPATH "@loader_path"
        )
    endforeach()
endif()

# ==============================================================================
# Generate cmake config files
# ==============================================================================

include(CMakePackageConfigHelpers)

# Configure config file with built components info
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/Config.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
)

# Generate version file
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY AnyNewerVersion
)

# Install config files
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/tanhConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    COMPONENT dev
)

# Install targets export
install(EXPORT "tanhTargets"
    FILE tanhTargets.cmake
    NAMESPACE ${PROJECT_NAME}::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}
    COMPONENT dev
)