cmake_minimum_required(VERSION 3.24)
project(ProjectWisp VERSION 0.1 LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
include(FetchContent)
set(FETCHCONTENT_QUIET ON)

find_package(Vulkan REQUIRED)
find_program(GLSLANG_VALIDATOR NAMES glslangValidator glslang REQUIRED
  HINTS $ENV{VULKAN_SDK}/bin)

# ---- third-party (FetchContent) ----
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.18 GIT_SHALLOW TRUE)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.91.9 GIT_SHALLOW TRUE)
FetchContent_Declare(vkbootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git GIT_TAG v1.4.307 GIT_SHALLOW TRUE)
FetchContent_Declare(vma GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git GIT_TAG v3.3.0 GIT_SHALLOW TRUE)
FetchContent_Declare(stb GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG 2c980bb59875b0d32144a71867fbdebb2f77cd20
  SOURCE_SUBDIR no-cmake)
set(WITH_CURL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libbigwig GIT_REPOSITORY https://github.com/dpryan79/libBigWig.git GIT_TAG 0.4.9 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(SDL3 imgui vkbootstrap vma stb libbigwig)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3 Vulkan::Vulkan)

# ---- htslib: system package via pkg-config (libhts-dev / brew htslib / vcpkg htslib) ----
find_package(PkgConfig)
if(PkgConfig_FOUND)
  pkg_check_modules(HTS IMPORTED_TARGET htslib)
endif()
if(NOT HTS_FOUND)
  find_path(HTS_INCLUDE htslib/faidx.h)
  find_library(HTS_LIB NAMES hts libhts)
  if(NOT HTS_INCLUDE OR NOT HTS_LIB)
    message(FATAL_ERROR "htslib not found: apt install libhts-dev / brew install htslib / vcpkg install htslib")
  endif()
  add_library(hts_fallback INTERFACE)
  target_include_directories(hts_fallback INTERFACE ${HTS_INCLUDE})
  target_link_libraries(hts_fallback INTERFACE ${HTS_LIB})
  add_library(PkgConfig::HTS ALIAS hts_fallback)
endif()

# ---- shaders -> embedded SPIR-V headers ----
set(SHADER_OUT ${CMAKE_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${SHADER_OUT})
set(SHADER_HEADERS)
foreach(S fog.vert fog.frag)
  string(REPLACE "." "_" VAR ${S})
  add_custom_command(OUTPUT ${SHADER_OUT}/${S}.h
    COMMAND ${GLSLANG_VALIDATOR} -V --vn ${VAR}_spv -o ${SHADER_OUT}/${S}.h ${CMAKE_SOURCE_DIR}/src/shaders/${S}
    DEPENDS ${CMAKE_SOURCE_DIR}/src/shaders/${S})
  list(APPEND SHADER_HEADERS ${SHADER_OUT}/${S}.h)
endforeach()

add_executable(wisp
  src/main.cpp
  src/render/vk_context.cpp src/render/fog.cpp src/render/png_write.cpp
  src/data/overview.cpp src/data/tracks.cpp src/data/genome.cpp
  src/ui/app.cpp src/ui/theme.cpp
  ${SHADER_HEADERS})
target_include_directories(wisp PRIVATE src ${SHADER_OUT} ${stb_SOURCE_DIR} ${vma_SOURCE_DIR}/include)
target_link_libraries(wisp PRIVATE imgui vk-bootstrap::vk-bootstrap Vulkan::Vulkan PkgConfig::HTS libBigWig::libbigwig)
target_compile_definitions(wisp PRIVATE WISP_ASSET_DIR="${CMAKE_SOURCE_DIR}/assets")
if(MSVC)
  target_compile_options(wisp PRIVATE /W3)
else()
  target_compile_options(wisp PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-unused-parameter)
endif()
# third-party header implementation compiled in our TU: keep its warnings out of our build log
if(NOT MSVC)
  set_source_files_properties(src/render/vk_context.cpp PROPERTIES COMPILE_OPTIONS "-Wno-unused-variable")
endif()
