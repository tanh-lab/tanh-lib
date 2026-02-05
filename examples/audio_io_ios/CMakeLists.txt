cmake_minimum_required(VERSION 3.15)

project(audio_io_ios)

# This must be an iOS app
if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This example is only for iOS")
endif()


# Create the iOS app bundle
add_executable(${PROJECT_NAME} MACOSX_BUNDLE
    main.mm
    AppDelegate.mm
    ViewController.mm
)

# Link against the audio_io library
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        tanh::AudioIO
        tanh::Core
)

# iOS-specific frameworks (UIKit and Foundation are needed for the UI layer)
target_link_libraries(${PROJECT_NAME}
    PRIVATE
        "-framework UIKit"
        "-framework Foundation"
)

# Set bundle properties
set_target_properties(${PROJECT_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    MACOSX_BUNDLE_BUNDLE_NAME ${PROJECT_NAME}
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.tanh.audio_io_ios"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.tanh.audio_io_ios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2" # iPhone and iPad
    # Use manual code signing to allow building without a team (for simulator)
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Manual"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY ""
    XCODE_ATTRIBUTE_DEVELOPMENT_TEAM ""
)

