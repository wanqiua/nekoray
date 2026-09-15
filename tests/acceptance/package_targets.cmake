# Optional package acceptance drivers. They are copied into an already verified
# disposable payload and must never be shipped as part of the portable ZIP.
if (NOT WIN32)
    return()
endif ()

set(PACKAGE_ACCEPTANCE_DIR "${CMAKE_CURRENT_LIST_DIR}")
if (EXISTS "${PACKAGE_ACCEPTANCE_DIR}/package_gui_probe.cpp")
    add_executable(nekoray_package_probe EXCLUDE_FROM_ALL
            "${PACKAGE_ACCEPTANCE_DIR}/package_gui_probe.cpp"
            ${GUI_TEST_SOURCES} ${CMAKE_BINARY_DIR}/translations.qrc)
    set_target_properties(nekoray_package_probe PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON)
    target_link_libraries(nekoray_package_probe PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network Qt${QT_VERSION_MAJOR}::Svg
            Threads::Threads ${NKR_EXTERNAL_TARGETS} ${PLATFORM_LIBRARIES} shell32)
    if (MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
        target_compile_options(nekoray_package_probe PRIVATE
                "/FI${CMAKE_SOURCE_DIR}/3rdparty/qt_msvc_compat.h")
    endif ()
endif ()

if (EXISTS "${PACKAGE_ACCEPTANCE_DIR}/package_smoke.cpp")
    add_executable(nekoray_package_smoke EXCLUDE_FROM_ALL "${PACKAGE_ACCEPTANCE_DIR}/package_smoke.cpp")
    target_link_libraries(nekoray_package_smoke PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets Qt${QT_VERSION_MAJOR}::Network)
    if (MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
        target_compile_options(nekoray_package_smoke PRIVATE
                "/FI${CMAKE_SOURCE_DIR}/3rdparty/qt_msvc_compat.h")
    endif ()
endif ()
