# OBS CMake Windows Architecture Helper

include_guard(GLOBAL)

include(compilerconfig)

# Upstream configures child builds here (build_x86, and build_x64 on ARM64) for the 32-bit and x64 game capture hook
# binaries. Knurbs ships OBS Studio's signed hook binaries instead (plugins/win-capture/cmake/obs-hook.cmake), so the
# parent architecture is the only one built.
if(NOT DEFINED OBS_PARENT_ARCHITECTURE)
  if(CMAKE_VS_PLATFORM_NAME MATCHES "(Win32|x64|ARM64)")
    set(OBS_PARENT_ARCHITECTURE ${CMAKE_VS_PLATFORM_NAME})
  else()
    message(FATAL_ERROR "Unsupported generator platform for Windows builds: ${CMAKE_VS_PLATFORM_NAME}!")
  endif()
endif()

if(NOT CMAKE_VS_PLATFORM_NAME STREQUAL x64)
  message(FATAL_ERROR "Knurbs builds for x64 only (the pinned OBS Studio hook binaries are the x64 release's).")
endif()

if(NOT OBS_PARENT_ARCHITECTURE STREQUAL CMAKE_VS_PLATFORM_NAME)
  message(FATAL_ERROR "Child architecture builds are no longer used: remove OBS_PARENT_ARCHITECTURE from the cache.")
endif()
