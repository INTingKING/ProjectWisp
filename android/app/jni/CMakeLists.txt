cmake_minimum_required(VERSION 3.24)
project(WispAndroid)
# The whole desktop project (SDL3 via FetchContent, imgui, vk-bootstrap, libBigWig, htslib) builds as libmain.so + libSDL3.so.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../.. wisp)
