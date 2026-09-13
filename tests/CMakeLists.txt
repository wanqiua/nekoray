cmake_minimum_required(VERSION 3.16)
project(nekoray_batch_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
enable_testing()
add_executable(speedtest_batch_test speedtest_batch_test.cpp)
target_link_libraries(speedtest_batch_test PRIVATE Threads::Threads)
add_test(NAME speedtest_batch COMMAND speedtest_batch_test)

if (WIN32 AND TARGET Qt${QT_VERSION_MAJOR}::Core)
    add_executable(owned_elevated_process_test
            owned_elevated_process_test.cpp ../sys/OwnedElevatedProcess.cpp)
    target_include_directories(owned_elevated_process_test PRIVATE ..)
    target_link_libraries(owned_elevated_process_test PRIVATE Qt${QT_VERSION_MAJOR}::Core shell32)
    if (MSVC AND MSVC_VERSION GREATER_EQUAL 1950)
        target_compile_options(owned_elevated_process_test PRIVATE "/FI${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/qt_msvc_compat.h")
    endif ()
    add_test(NAME owned_elevated_process COMMAND owned_elevated_process_test)
    set_tests_properties(owned_elevated_process PROPERTIES
            ENVIRONMENT "QT_FORCE_STDERR_LOGGING=1" TIMEOUT 20)
endif ()
