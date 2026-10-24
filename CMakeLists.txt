cmake_minimum_required(VERSION 3.16)
project(llcast C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBAV REQUIRED IMPORTED_TARGET libavcodec libavutil libswscale)

add_executable(llcast_encode src/encode_nvenc.c src/capture.c src/overlay.c)
target_link_libraries(llcast_encode PkgConfig::LIBAV)
target_compile_options(llcast_encode PRIVATE -Wall -Wextra -O2)

pkg_check_modules(GST QUIET IMPORTED_TARGET
  gstreamer-1.0 gstreamer-app-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 gio-2.0)
if(GST_FOUND)
  add_executable(llcast_whep src/whep_server.c)
  target_link_libraries(llcast_whep PkgConfig::GST)
  target_compile_options(llcast_whep PRIVATE -Wall -Wextra -O2)
else()
  message(STATUS "GStreamer dev headers not found; llcast_whep will not be built")
endif()
