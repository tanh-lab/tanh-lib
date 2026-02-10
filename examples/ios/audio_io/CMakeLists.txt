if(NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    message(FATAL_ERROR "This example is only for iOS")
endif()

set(TARGET_NAME audio_io)

add_executable(${TARGET_NAME} MACOSX_BUNDLE
    main.mm
    AppDelegate.mm
    ViewController.mm
)

target_link_libraries(${TARGET_NAME}
    PRIVATE
        tanh::AudioIO
        tanh::Core
        "-framework UIKit"
        "-framework Foundation"
)

# Code signing configuration
# Pass -DDEVELOPMENT_TEAM=YOUR_TEAM_ID for automatic signing
# Requires: Xcode > Settings > Accounts has your Apple ID logged in
if(CMAKE_OSX_SYSROOT MATCHES "iphoneos")
    set(CODE_SIGN_STYLE "Automatic")
    set(CODE_SIGN_IDENTITY "Apple Development")
else()
    set(CODE_SIGN_STYLE "Manual")
    set(CODE_SIGN_IDENTITY "")
endif()

set_target_properties(${TARGET_NAME} PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist
    MACOSX_BUNDLE_BUNDLE_NAME ${TARGET_NAME}
    MACOSX_BUNDLE_GUI_IDENTIFIER "com.tanh.audioio-ios"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0"
    XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "com.tanh.audioio-ios"
    XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2" # iPhone and iPad
    XCODE_ATTRIBUTE_CODE_SIGN_STYLE "${CODE_SIGN_STYLE}"
    XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "${CODE_SIGN_IDENTITY}"
)

if(DEVELOPMENT_TEAM)
    set_target_properties(${TARGET_NAME} PROPERTIES
        XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "${DEVELOPMENT_TEAM}"
    )
endif()
