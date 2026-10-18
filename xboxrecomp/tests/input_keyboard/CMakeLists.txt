cmake_minimum_required(VERSION 3.20)
project(input_keyboard_regression C)
# The key table and the one key map (src/input/keyboard.c). No SDL and no
# window, so it builds and runs on every host.
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(input_keyboard_test test_main.c ../../src/input/keyboard.c)
target_include_directories(input_keyboard_test PRIVATE ../../src/input ../../src)
add_test(NAME input_keyboard COMMAND input_keyboard_test)
