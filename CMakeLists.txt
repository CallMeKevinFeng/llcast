cmake_minimum_required(VERSION 3.16)
project(llcast C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

add_executable(llcast_encode src/encode_nvenc.c src/capture.c)
target_link_libraries(llcast_encode PkgConfig::LIBAV)
target_compile_options(llcast_encode PRIVATE -Wall -Wextra -O2)
