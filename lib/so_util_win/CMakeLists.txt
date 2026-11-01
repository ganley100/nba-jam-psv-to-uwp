cmake_minimum_required(VERSION 3.14)
project(so_util_win C)

add_library(so_util_win STATIC
    so_util_win.c
)

target_include_directories(so_util_win PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (MSVC)
    target_compile_definitions(so_util_win PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(so_util_win PRIVATE /W3)
else()
    target_compile_options(so_util_win PRIVATE -Wall -Wextra)
endif()

# Tiny smoke-test executable (optional)
option(SO_UTIL_WIN_BUILD_TEST "Build so_util_win_test" ON)
if (SO_UTIL_WIN_BUILD_TEST)
    add_executable(so_util_win_test test_main.c)
    target_link_libraries(so_util_win_test PRIVATE so_util_win)
endif()
