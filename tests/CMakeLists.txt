add_test(NAME download_asset
    COMMAND ${CMAKE_COMMAND} "-DTEST_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/download_asset"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/check_download_asset.cmake)

function(swiftify_test name)
    qt_add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE swiftify_app Qt6::Test)
    add_test(NAME ${name} COMMAND ${name})
    # on linux, offscreen lacks hardware opengl/vulkan, breaking qrhi in tst_views
    if(NOT SWIFTIFY_IS_LINUX OR NOT name STREQUAL "tst_views")
        set_tests_properties(${name} PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endfunction()

swiftify_test(tst_settings)
swiftify_test(tst_discord FakeCore.h)
swiftify_test(tst_media_timeline)
swiftify_test(tst_translations)
swiftify_test(tst_theme)
swiftify_test(tst_playback FakeCore.h)
swiftify_test(tst_palette)
swiftify_test(tst_spectrum FakeCore.h)
swiftify_test(tst_covers)
swiftify_test(tst_views FakeCore.h)
swiftify_test(tst_local FakeCore.h)
swiftify_test(tst_session FakeCore.h)
if(SWIFTIFY_SOLOIST)
    swiftify_test(tst_soloist FakeCore.h)
endif()
# frames are read back through qrhi, which is only offered as a private api
# only this test binary, which is rebuilt with qt anyway
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 REQUIRED COMPONENTS GuiPrivate)
target_link_libraries(tst_views PRIVATE swiftify_appplugin Qt6::GuiPrivate)
if(WIN32)
    find_program(SWIFTIFY_POWERSHELL NAMES powershell pwsh)
    if(SWIFTIFY_POWERSHELL)
        add_test(NAME windows_build_script
            COMMAND ${SWIFTIFY_POWERSHELL} -NoProfile -ExecutionPolicy Bypass
                -File ${CMAKE_CURRENT_SOURCE_DIR}/check_windows_build.ps1)
    endif()
    swiftify_test(tst_windows FakeCore.h)
    target_link_libraries(tst_windows PRIVATE swiftify_appplugin Qt6::GuiPrivate)
endif()

# what a frame of the main window costs: run by hand, not by ctest
find_package(Qt6 REQUIRED COMPONENTS QuickPrivate)
qt_add_executable(bench_frames bench_frames.cpp FakeCore.h ViewRig.h)
target_link_libraries(bench_frames PRIVATE swiftify_app swiftify_appplugin Qt6::Test Qt6::GuiPrivate Qt6::QuickPrivate)

# every material symbol the qml names exists in the pinned font
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND AND SWIFTIFY_ICON_CODEPOINTS)
    add_test(NAME icon_names
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_icon_names.py
            ${SWIFTIFY_ICON_CODEPOINTS} ${PROJECT_SOURCE_DIR}/qml)
endif()
# a script line opening with a bracket runs on from the one before
if(Python3_FOUND)
    add_test(NAME translations_catalog
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_translations.py ${PROJECT_SOURCE_DIR})
    add_test(NAME qml_statements
        COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/check_qml_statements.py ${PROJECT_SOURCE_DIR}/qml)
endif()


swiftify_test(tst_menus FakeCore.h ViewRig.h)
target_link_libraries(tst_menus PRIVATE swiftify_appplugin Qt6::GuiPrivate)

swiftify_test(tst_segmented FakeCore.h ViewRig.h)
target_link_libraries(tst_segmented PRIVATE swiftify_appplugin Qt6::GuiPrivate)
