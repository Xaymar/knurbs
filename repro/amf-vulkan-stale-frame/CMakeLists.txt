# Standalone reproducer: AMF on a Vulkan context encodes the picture of an earlier input.
#
#   cmake -S . -B build -D AMF_INCLUDE_DIR=<dir with core/Factory.h> -D FFMPEG_DIR=<FFmpeg dev build>
#         [-D VULKAN_HEADERS_DIR=<Vulkan-Headers checkout>]
#   cmake --build build --config Release
#
# Needs: the Vulkan headers, the AMF SDK headers (amf/public/include of GPUOpen-LibrariesAndSDKs/AMF) and FFmpeg's
# libavcodec (any dev build: include/ + lib/ with avcodec.lib, avutil.lib) to decode and check the stream.
# vulkan-1.dll and amfrt64.dll (the driver's AMF runtime) are loaded at run time: no Vulkan SDK or AMF import library.
# Without AMF_INCLUDE_DIR / VULKAN_HEADERS_DIR the headers are downloaded.
cmake_minimum_required(VERSION 3.20)
project(amf_vulkan_stale_frame CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(FetchContent)

set(VULKAN_HEADERS_DIR "" CACHE PATH "Vulkan-Headers checkout (contains include/vulkan/vulkan.h)")
if(VULKAN_HEADERS_DIR)
  set(VK_INCLUDE "${VULKAN_HEADERS_DIR}/include")
else()
  find_package(Vulkan QUIET)
  if(Vulkan_INCLUDE_DIRS)
    set(VK_INCLUDE "${Vulkan_INCLUDE_DIRS}")
  else()
    FetchContent_Declare(vulkan_headers GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
                                        GIT_TAG main GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(vulkan_headers)
    set(VK_INCLUDE "${vulkan_headers_SOURCE_DIR}/include")
  endif()
endif()

set(AMF_INCLUDE_DIR "" CACHE PATH "AMF SDK include directory (contains core/Factory.h)")
if(NOT AMF_INCLUDE_DIR)
  FetchContent_Declare(amf_sdk GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/AMF.git
                               GIT_TAG master GIT_SHALLOW TRUE)
  FetchContent_GetProperties(amf_sdk)
  if(NOT amf_sdk_POPULATED)
    FetchContent_Populate(amf_sdk)
  endif()
  set(AMF_INCLUDE_DIR "${amf_sdk_SOURCE_DIR}/amf/public/include")
endif()

set(FFMPEG_DIR "" CACHE PATH "FFmpeg dev build (include/libavcodec, lib/avcodec.lib)")
find_path(AVCODEC_INCLUDE libavcodec/avcodec.h HINTS "${FFMPEG_DIR}/include" REQUIRED)
find_library(AVCODEC_LIB avcodec HINTS "${FFMPEG_DIR}/lib" REQUIRED)
find_library(AVUTIL_LIB avutil HINTS "${FFMPEG_DIR}/lib" REQUIRED)

add_executable(amf-vulkan-stale-frame main.cpp)
target_include_directories(amf-vulkan-stale-frame PRIVATE "${VK_INCLUDE}" "${AMF_INCLUDE_DIR}" "${AVCODEC_INCLUDE}")
target_link_libraries(amf-vulkan-stale-frame PRIVATE "${AVCODEC_LIB}" "${AVUTIL_LIB}" d3d11 dxgi)
target_compile_definitions(amf-vulkan-stale-frame PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
  target_compile_options(amf-vulkan-stale-frame PRIVATE /W3 /utf-8)
endif()

# Copy FFmpeg's DLLs next to the executable when they are in FFMPEG_DIR/bin.
file(GLOB FFMPEG_DLLS "${FFMPEG_DIR}/bin/avcodec*.dll" "${FFMPEG_DIR}/bin/avutil*.dll" "${FFMPEG_DIR}/bin/swresample*.dll")
foreach(dll ${FFMPEG_DLLS})
  add_custom_command(TARGET amf-vulkan-stale-frame POST_BUILD
                     COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:amf-vulkan-stale-frame>")
endforeach()
