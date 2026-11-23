cmake_minimum_required(VERSION 3.28...3.30)

# D3D11 producers (Windows Graphics Capture, DXGI desktop duplication) on the Vulkan renderer: a private D3D11 device on
# an adapter picked by LUID, and a set of keyed-mutex NT-handle textures imported once into Vulkan
# (docs/graphics-interop.md, "Windows capture").
add_library(d3d11-vulkan-bridge INTERFACE)
add_library(OBS::d3d11-vulkan-bridge ALIAS d3d11-vulkan-bridge)

target_sources(
  d3d11-vulkan-bridge
  INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/d3d11-vulkan-bridge/d3d11-vulkan-bridge.cpp"
    d3d11-vulkan-bridge/d3d11-vulkan-bridge.hpp
)

target_include_directories(d3d11-vulkan-bridge INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(d3d11-vulkan-bridge INTERFACE d3d11 dxgi)

set_target_properties(d3d11-vulkan-bridge PROPERTIES FOLDER deps)
