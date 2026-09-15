set(PLATFORM_SOURCES 3rdparty/WinCommander.cpp sys/windows/guihelper.cpp sys/windows/MiniDump.cpp sys/WindowsSystemProxy.cpp)
set(PLATFORM_LIBRARIES wininet wsock32 ws2_32 user32 rasapi32 iphlpapi)

include(cmake/windows/generate_product_version.cmake)
generate_product_version(
        QV2RAY_RC
        ICON "${CMAKE_SOURCE_DIR}/res/nekoray.ico"
        NAME "NekoRay_BY_WanQiu"
        BUNDLE "NekoRay_BY_WanQiu"
        VERSION_MAJOR ${PROJECT_VERSION_MAJOR}
        VERSION_MINOR ${PROJECT_VERSION_MINOR}
        VERSION_PATCH 0
        VERSION_REVISION 0
        COMPANY_NAME "WanQiu"
        COMPANY_COPYRIGHT "NekoRay contributors / WanQiu"
        FILE_DESCRIPTION "NekoRay_BY_WanQiu"
        ORIGINAL_FILENAME "NekoRay_BY_WanQiu.exe"
)
add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX)
set(GUI_TYPE WIN32)
if (MINGW)
    if (NOT DEFINED MinGW_ROOT)
        set(MinGW_ROOT "C:/msys64/mingw64")
    endif ()
else ()
    add_compile_options("/utf-8")
    add_compile_options("/std:c++17")
    add_definitions(-D_WIN32_WINNT=0x600 -D_SCL_SECURE_NO_WARNINGS -D_CRT_SECURE_NO_WARNINGS)
endif ()
