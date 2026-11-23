cmake_minimum_required(VERSION 3.28)
project(dmabuf-import-with-export-info CXX)
# Standalone (not part of the OBS build): cmake -S . -B build && cmake --build build && build/repro
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
pkg_check_modules(DRM REQUIRED IMPORTED_TARGET libdrm)
add_executable(repro main.cpp)
target_link_libraries(repro PRIVATE Vulkan::Vulkan PkgConfig::GBM PkgConfig::DRM)
