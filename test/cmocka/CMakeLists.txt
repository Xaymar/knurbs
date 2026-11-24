project(obs-cmocka)

find_package(CMocka CONFIG REQUIRED)

# obs.dll and the obs-deps runtime libraries are not next to the test executables on Windows
set(_obs_runtime_env "")
if(OS_WINDOWS)
  list(APPEND _obs_runtime_env "PATH=path_list_prepend:${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND _obs_runtime_env "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
endif()

# Every unit test and what it needs, so run-coverage.ps1 can build them in one go
add_custom_target(obs-unit-tests)

# obs_add_unit_test(<name>): <name>.c linked with libobs and cmocka, run in its own scratch directory
function(obs_add_unit_test name)
  add_executable(${name} ${name}.c)
  target_include_directories(${name} PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(${name} PRIVATE OBS::libobs cmocka::cmocka $<$<NOT:$<PLATFORM_ID:Windows>>:m>)
  set_target_properties(${name} PROPERTIES FOLDER "Tests and Examples")
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${name}")
  file(MAKE_DIRECTORY "${work_dir}")
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${work_dir}")
  set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
  add_dependencies(obs-unit-tests ${name})
endfunction()

# obs_add_graphics_test(<name>): a unit test that runs gs_* on the libobs-null module (see gs-test.h)
function(obs_add_graphics_test name)
  obs_add_unit_test(${name})
  target_compile_definitions(
    ${name}
    PRIVATE
      NULL_GRAPHICS_MODULE="$<TARGET_FILE:libobs-null>"
      OBS_DATA_DIR="${CMAKE_SOURCE_DIR}/libobs/data"
      TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  )
  add_dependencies(${name} libobs-null)
endfunction()

# obs_add_core_test(<name>): a graphics test that starts libobs (see obs-core-test.h). libobs finds its effects
# in ../../data/libobs relative to the working directory on Windows and in share/obs/libobs on Linux, so the test
# runs in work/<name>/bin/64bit next to a copy of the data.
function(obs_add_core_test name)
  obs_add_graphics_test(${name})
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${name}/bin/64bit")
  file(MAKE_DIRECTORY "${work_dir}")
  set_tests_properties(${name} PROPERTIES WORKING_DIRECTORY "${work_dir}")
  add_custom_command(
    TARGET ${name}
    POST_BUILD
    COMMAND
      "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/libobs/data"
      "${CMAKE_CURRENT_BINARY_DIR}/work/${name}/data/libobs"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_SOURCE_DIR}/libobs/data" "${work_dir}/share/obs/libobs"
    VERBATIM
  )
endfunction()

# Serializer test
add_executable(test_serializer test_serializer.c)
target_include_directories(test_serializer PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_serializer PRIVATE OBS::libobs cmocka::cmocka)
add_test(NAME test_serializer COMMAND test_serializer)

# darray test
add_executable(test_darray test_darray.c)
target_include_directories(test_darray PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_darray PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_darray COMMAND test_darray)

# bitstream test
add_executable(test_bitstream test_bitstream.c)
target_include_directories(test_bitstream PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_bitstream PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_bitstream COMMAND test_bitstream)

# Source ID aliases and settings migration
add_executable(test_source_alias test_source_alias.c)
target_include_directories(test_source_alias PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_source_alias PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_source_alias COMMAND test_source_alias)
set_tests_properties(test_source_alias PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")

# Video Device Capture: backend selection and show/hide, with fake backends
add_executable(
  test_video_device
  test_video_device.c
  ${CMAKE_SOURCE_DIR}/plugins/knurbs-video-device-capture/video-device.c
)
target_include_directories(
  test_video_device
  PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/plugins/knurbs-video-device-capture
)
target_link_libraries(test_video_device PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_video_device COMMAND test_video_device)
set_tests_properties(test_video_device PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")

# PipeWire async frame plane layout
if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  find_package(PipeWire 0.3.33 QUIET)
  find_package(Libdrm QUIET)
  if(PipeWire_FOUND AND Libdrm_FOUND)
    get_target_property(_libdrm_includes Libdrm::Libdrm INTERFACE_INCLUDE_DIRECTORIES)
    add_executable(test_pipewire_formats test_pipewire_formats.c ${CMAKE_SOURCE_DIR}/shared/pipewire/formats.c)
    target_include_directories(
      test_pipewire_formats
      PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/shared/pipewire ${_libdrm_includes}
    )
    target_link_libraries(test_pipewire_formats PRIVATE OBS::libobs PipeWire::PipeWire cmocka::cmocka)
    add_test(NAME test_pipewire_formats COMMAND test_pipewire_formats)
    set_tests_properties(test_pipewire_formats PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
  endif()
endif()

add_executable(test_no_video test_no_video.c)
target_include_directories(test_no_video PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_no_video PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_no_video test_no_video)

add_executable(test_type_alias test_type_alias.c)
target_include_directories(test_type_alias PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_type_alias PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_type_alias test_type_alias)

add_executable(test_encoder_patent_notice test_encoder_patent_notice.c)
target_include_directories(test_encoder_patent_notice PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_encoder_patent_notice PRIVATE OBS::libobs cmocka::cmocka)

add_test(test_encoder_patent_notice test_encoder_patent_notice)

# PCI vendor lookup of the VA-API plugin, against a fake sysfs tree.
if(OS_LINUX OR OS_FREEBSD OR OS_OPENBSD)
  add_executable(test_vaapi_vendor test_vaapi_vendor.c "${CMAKE_SOURCE_DIR}/plugins/knurbs-vaapi/vaapi-vendor.c")
  target_include_directories(test_vaapi_vendor PRIVATE ${CMOCKA_INCLUDE_DIR} "${CMAKE_SOURCE_DIR}/plugins/knurbs-vaapi")
  target_link_libraries(test_vaapi_vendor PRIVATE OBS::libobs cmocka::cmocka)

  add_test(test_vaapi_vendor test_vaapi_vendor)
endif()

# Vulkan backend export test
if(ENABLE_VULKAN)
  find_package(Vulkan REQUIRED)
  add_executable(test_vulkan_exports test_vulkan_exports.cpp)
  target_include_directories(test_vulkan_exports PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(
    test_vulkan_exports
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_exports libobs-vulkan)
  if(TARGET libobs-null)
    target_compile_definitions(test_vulkan_exports PRIVATE NULL_GRAPHICS_MODULE="$<TARGET_FILE:libobs-null>")
    add_dependencies(test_vulkan_exports libobs-null)
  endif()
  target_link_libraries(test_vulkan_exports PRIVATE OBS::libobs cmocka::cmocka)
  add_test(test_vulkan_exports test_vulkan_exports)

  # Needs a Vulkan device; each test skips when device_create reports GS_ERROR_NOT_SUPPORTED.
  add_executable(test_vulkan_device test_vulkan_device.cpp)
  target_include_directories(test_vulkan_device PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_device
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_device libobs-vulkan)
  target_link_libraries(test_vulkan_device PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_device test_vulkan_device)

  # Queue global priority: default, opt-in realtime, refusal fallback; skips without a device.
  add_executable(test_vulkan_priority test_vulkan_priority.cpp)
  target_include_directories(test_vulkan_priority PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_priority
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_priority libobs-vulkan)
  target_link_libraries(test_vulkan_priority PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_priority test_vulkan_priority)

  # Textures, clear and stage-surface readback; skips without a device.
  add_executable(test_vulkan_readback test_vulkan_readback.cpp)
  target_include_directories(test_vulkan_readback PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_readback
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_readback libobs-vulkan)
  target_link_libraries(test_vulkan_readback PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_readback test_vulkan_readback)

  # Texture upload, mipmaps, dynamic map and sRGB views; skips without a device.
  add_executable(test_vulkan_upload test_vulkan_upload.cpp)
  target_include_directories(test_vulkan_upload PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_upload
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_upload libobs-vulkan)
  target_link_libraries(test_vulkan_upload PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_upload test_vulkan_upload)

  # Swapchain and present on hidden Win32 windows, or Wayland toplevels on a headless weston the test starts; skips
  # without a device or windowing.
  add_executable(test_vulkan_swapchain test_vulkan_swapchain.cpp)
  target_include_directories(test_vulkan_swapchain PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_swapchain
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  add_dependencies(test_vulkan_swapchain libobs-vulkan)
  target_link_libraries(test_vulkan_swapchain PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  if(OS_LINUX)
    # Wayland: xdg toplevels on a headless weston the test starts, read back through weston-screenshooter (PNG).
    find_package(PkgConfig REQUIRED)
    find_package(PNG REQUIRED)
    pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
    pkg_get_variable(_wayland_protocols wayland-protocols pkgdatadir)
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    set(_xdg_shell "${_wayland_protocols}/stable/xdg-shell/xdg-shell.xml")
    set(_gen "${CMAKE_CURRENT_BINARY_DIR}/wayland")
    add_custom_command(
      OUTPUT "${_gen}/xdg-shell-client-protocol.h" "${_gen}/xdg-shell-protocol.c"
      COMMAND "${CMAKE_COMMAND}" -E make_directory "${_gen}"
      COMMAND "${WAYLAND_SCANNER}" client-header "${_xdg_shell}" "${_gen}/xdg-shell-client-protocol.h"
      COMMAND "${WAYLAND_SCANNER}" private-code "${_xdg_shell}" "${_gen}/xdg-shell-protocol.c"
      DEPENDS "${_xdg_shell}"
      VERBATIM
    )
    target_sources(test_vulkan_swapchain PRIVATE "${_gen}/xdg-shell-protocol.c" "${_gen}/xdg-shell-client-protocol.h")
    target_include_directories(test_vulkan_swapchain PRIVATE "${_gen}")
    target_link_libraries(test_vulkan_swapchain PRIVATE PkgConfig::WAYLAND_CLIENT PNG::PNG)
  endif()
  add_test(test_vulkan_swapchain test_vulkan_swapchain)

  # Shader translation and params; the effect parser is compiled in (ep_parse is not exported). No device needed
  # except for the glslang refcount test, which skips without one.
  add_executable(
    test_vulkan_shader
    test_vulkan_shader.cpp
    test_vulkan_shader_effect.c
    test_vulkan_shader_effect.h
    ${CMAKE_SOURCE_DIR}/libobs/graphics/effect-parser.c
  )
  target_include_directories(test_vulkan_shader PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_shader
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_shader PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_shader libobs-vulkan)
  target_link_libraries(test_vulkan_shader PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_shader test_vulkan_shader)

  # Draw path; needs a device (skips without one). The effect parser is compiled in, as for test_vulkan_shader.
  add_executable(
    test_vulkan_draw
    test_vulkan_draw.cpp
    test_vulkan_shader_effect.c
    test_vulkan_shader_effect.h
    ${CMAKE_SOURCE_DIR}/libobs/graphics/effect-parser.c
  )
  target_include_directories(test_vulkan_draw PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_draw
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_draw PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_draw libobs-vulkan)
  target_link_libraries(test_vulkan_draw PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_draw test_vulkan_draw)

  # NV12/P010 two-plane textures: plane-view rendering, sampling, copy and staging; skips without a device.
  add_executable(test_vulkan_nv12 test_vulkan_nv12.cpp)
  target_include_directories(test_vulkan_nv12 PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(test_vulkan_nv12 PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan")
  target_compile_features(test_vulkan_nv12 PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_nv12 libobs-vulkan)
  target_link_libraries(test_vulkan_nv12 PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_nv12 test_vulkan_nv12)

  # Probe (pure Vulkan, every device): may a compute shader write the planes of NV12/P010/4:2:2 images through
  # per-plane storage views? Logs the answer per format; skips without a loader.
  add_executable(test_vulkan_yuv_storage test_vulkan_yuv_storage.cpp)
  target_include_directories(test_vulkan_yuv_storage PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_features(test_vulkan_yuv_storage PRIVATE cxx_std_17)
  target_link_libraries(test_vulkan_yuv_storage PRIVATE Vulkan::Vulkan cmocka::cmocka)
  add_test(NAME test_vulkan_yuv_storage COMMAND test_vulkan_yuv_storage)

  # Compute YUV conversion against the fragment passes (NV12/P010/P216 at three sizes, within 1 LSB) and GPU timings
  # of both; skips without a device.
  add_executable(test_vulkan_yuv_compute test_vulkan_yuv_compute.cpp)
  target_include_directories(test_vulkan_yuv_compute PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(
    test_vulkan_yuv_compute
    PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}" OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_yuv_compute PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_yuv_compute libobs-vulkan)
  target_link_libraries(test_vulkan_yuv_compute PRIVATE OBS::libobs cmocka::cmocka)
  add_test(NAME test_vulkan_yuv_compute COMMAND test_vulkan_yuv_compute)

  if(OS_WINDOWS)
    # External memory and sync (D3D11 import, keyed mutex, export/import across devices and processes); needs a
    # device (skips without one).
    add_executable(test_vulkan_interop test_vulkan_interop.cpp)
    target_include_directories(test_vulkan_interop PRIVATE ${CMOCKA_INCLUDE_DIR})
    target_compile_definitions(
      test_vulkan_interop
      PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
    )
    target_compile_features(test_vulkan_interop PRIVATE cxx_std_17)
    add_dependencies(test_vulkan_interop libobs-vulkan)
    target_link_libraries(test_vulkan_interop PRIVATE OBS::libobs cmocka::cmocka d3d11 dxgi)
    add_test(test_vulkan_interop test_vulkan_interop)

    # The D3D11 capture bridge (shared/d3d11-vulkan-bridge) across simulated device losses: every keyed-mutex slot is
    # writable again after the rebuild. Needs a device (skips without one).
    if(TARGET OBS::d3d11-vulkan-bridge)
      add_executable(test_vulkan_d3d11_bridge test_vulkan_d3d11_bridge.cpp)
      target_include_directories(
        test_vulkan_d3d11_bridge
        PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan
      )
      target_compile_definitions(
        test_vulkan_d3d11_bridge
        PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
      )
      target_compile_features(test_vulkan_d3d11_bridge PRIVATE cxx_std_17)
      add_dependencies(test_vulkan_d3d11_bridge libobs-vulkan)
      target_link_libraries(
        test_vulkan_d3d11_bridge
        PRIVATE OBS::libobs OBS::vma Vulkan::Headers OBS::d3d11-vulkan-bridge cmocka::cmocka
      )
      add_test(NAME test_vulkan_d3d11_bridge COMMAND test_vulkan_d3d11_bridge)
      set_tests_properties(
        test_vulkan_d3d11_bridge
        PROPERTIES
          ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
          ENVIRONMENT_MODIFICATION "${_obs_runtime_env}"
          SKIP_REGULAR_EXPRESSION "SKIPPED"
      )
    endif()
  endif()

  if(OS_LINUX)
    # Linux external memory and sync (dmabufs, GBM, opaque fds, sync_files, DRM syncobjs, SCM_RIGHTS to a child);
    # needs a device (skips without one).
    find_package(Libdrm REQUIRED)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GBM REQUIRED IMPORTED_TARGET gbm)
    add_executable(test_vulkan_interop_linux test_vulkan_interop_linux.cpp)
    target_include_directories(test_vulkan_interop_linux PRIVATE ${CMOCKA_INCLUDE_DIR})
    target_compile_definitions(
      test_vulkan_interop_linux
      PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
    )
    target_compile_features(test_vulkan_interop_linux PRIVATE cxx_std_17)
    add_dependencies(test_vulkan_interop_linux libobs-vulkan)
    target_link_libraries(test_vulkan_interop_linux PRIVATE OBS::libobs cmocka::cmocka Libdrm::Libdrm PkgConfig::GBM)
    add_test(test_vulkan_interop_linux test_vulkan_interop_linux)
    set_tests_properties(
      test_vulkan_interop_linux
      PROPERTIES
        ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
  endif()

  # Depth/stencil buffers and depth-tested drawing; needs a device (skips without one).
  add_executable(test_vulkan_depth test_vulkan_depth.cpp)
  target_include_directories(test_vulkan_depth PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_definitions(
    test_vulkan_depth
    PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
  )
  target_compile_features(test_vulkan_depth PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_depth libobs-vulkan)
  target_link_libraries(test_vulkan_depth PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_depth test_vulkan_depth)

  # Native objects, timers, debug markers, cube textures, HDR format choice and fast clear (test_vulkan_features), and
  # the device-loss rebuild (test_vulkan_loss); need a device (skip without one).
  foreach(_test IN ITEMS test_vulkan_features test_vulkan_loss)
    add_executable(${_test} ${_test}.cpp)
    target_include_directories(${_test} PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
    target_compile_definitions(${_test} PRIVATE OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan")
    target_compile_features(${_test} PRIVATE cxx_std_17)
    add_dependencies(${_test} libobs-vulkan)
    target_link_libraries(${_test} PRIVATE OBS::libobs OBS::vma Vulkan::Headers cmocka::cmocka)
    add_test(${_test} ${_test})
  endforeach()

  # Every .effect in the tree and texrender through the generic gs_* API (gs_create); skips without a device.
  # The test walks the same directories at run time and must find exactly this many.
  file(
    GLOB_RECURSE _effects
    CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/libobs/data/*.effect"
    "${CMAKE_SOURCE_DIR}/frontend/data/*.effect"
    "${CMAKE_SOURCE_DIR}/test/test-input/data/*.effect"
  )
  file(GLOB _plugin_dirs LIST_DIRECTORIES true "${CMAKE_SOURCE_DIR}/plugins/*")
  foreach(_dir IN LISTS _plugin_dirs)
    file(GLOB_RECURSE _plugin_effects CONFIGURE_DEPENDS "${_dir}/data/*.effect")
    list(APPEND _effects ${_plugin_effects})
  endforeach()
  list(LENGTH _effects _effect_count)
  add_executable(test_vulkan_effects test_vulkan_effects.cpp)
  target_include_directories(test_vulkan_effects PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(
    test_vulkan_effects
    PRIVATE
      OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
      OBS_VULKAN_MODULE="$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan"
      OBS_EFFECT_COUNT=${_effect_count}
  )
  target_compile_features(test_vulkan_effects PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_effects libobs-vulkan)
  target_link_libraries(test_vulkan_effects PRIVATE OBS::libobs cmocka::cmocka)
  add_test(test_vulkan_effects test_vulkan_effects)

  # End-to-end OBS video pipeline on a module given as argument; rendered for Vulkan and (Windows) D3D11 in separate
  # processes, then compared. Only a Vulkan render without a device may skip (reported Skipped); it writes nothing,
  # and compare then fails on the missing file (or is not run), so the comparison never passes without comparing.
  add_executable(test_vulkan_video test_vulkan_video.cpp)
  target_include_directories(test_vulkan_video PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_compile_definitions(test_vulkan_video PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  target_compile_features(test_vulkan_video PRIVATE cxx_std_17)
  add_dependencies(test_vulkan_video libobs-vulkan)
  target_link_libraries(test_vulkan_video PRIVATE OBS::libobs cmocka::cmocka $<$<PLATFORM_ID:Windows>:gdi32>)
  set(_video_vulkan "${CMAKE_CURRENT_BINARY_DIR}/video_vulkan.bin")
  set(_video_d3d11 "${CMAKE_CURRENT_BINARY_DIR}/video_d3d11.bin")
  add_test(
    NAME test_vulkan_video_vulkan
    COMMAND test_vulkan_video render "$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan" vulkan "${_video_vulkan}"
  )
  # cmocka's skip() exits 0: SKIPPED in the output makes ctest report Skipped, never Passed.
  set_tests_properties(
    test_vulkan_video_vulkan
    PROPERTIES FIXTURES_SETUP video_vulkan SKIP_REGULAR_EXPRESSION "SKIPPED"
  )
  if(TARGET libobs-d3d11)
    add_dependencies(test_vulkan_video libobs-d3d11)
    add_test(
      NAME test_vulkan_video_d3d11
      COMMAND test_vulkan_video render "$<TARGET_FILE_DIR:libobs-d3d11>/libobs-d3d11" d3d11 "${_video_d3d11}"
    )
    set_tests_properties(
      test_vulkan_video_d3d11
      PROPERTIES FIXTURES_SETUP video_d3d11 SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
    add_test(NAME test_vulkan_video_compare COMMAND test_vulkan_video compare "${_video_vulkan}" "${_video_d3d11}")
    set_tests_properties(
      test_vulkan_video_compare
      PROPERTIES FIXTURES_REQUIRED "video_vulkan;video_d3d11" SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
  else()
    # No D3D11 reference outside Windows and no golden file committed: only the render's analytic checks run. Say
    # so in every ctest run instead of leaving the comparison out silently.
    add_test(
      NAME test_vulkan_video_compare
      COMMAND "${CMAKE_COMMAND}" -E echo "SKIPPED: no D3D11 reference render on this platform"
    )
    set_tests_properties(test_vulkan_video_compare PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")
  endif()

  # Queue priority measurement only (not a test, not installed): the synthetic D3D11 foreground load. The OBS side
  # load is `test_vulkan_video load`. See docs/superpowers/specs/2026-11-17-libobs-vulkan-priority-measurement.md.
  if(WIN32)
    add_executable(vulkan_priority_fg_load vulkan_priority_fg_load.cpp)
    target_link_libraries(vulkan_priority_fg_load PRIVATE d3d11 dxgi d3dcompiler)
  endif()

  # Pure logic: compiles the tracker directly, needs no device or loader.
  add_executable(test_vulkan_tracker test_vulkan_tracker.cpp ${CMAKE_SOURCE_DIR}/libobs-vulkan/vulkan-tracker.cpp)
  target_include_directories(test_vulkan_tracker PRIVATE ${CMOCKA_INCLUDE_DIR} ${CMAKE_SOURCE_DIR}/libobs-vulkan)
  target_compile_features(test_vulkan_tracker PRIVATE cxx_std_17)
  target_link_libraries(test_vulkan_tracker PRIVATE Vulkan::Headers cmocka::cmocka)
  add_test(test_vulkan_tracker test_vulkan_tracker)

  # Every test that creates a device runs under the validation layer with synchronization validation, so the hazard
  # checks behind the tracker run in every ctest invocation. Set OBS_VULKAN_REQUIRE_VALIDATION=1 to make a missing
  # validation layer fail the test. cmocka's skip() exits 0, so SKIPPED (no Vulkan 1.3 device) reports Skipped.
  set_tests_properties(
    test_vulkan_device
    test_vulkan_priority
    test_vulkan_readback
    test_vulkan_upload
    test_vulkan_swapchain
    test_vulkan_shader
    test_vulkan_draw
    test_vulkan_nv12
    test_vulkan_yuv_compute
    test_vulkan_depth
    test_vulkan_features
    test_vulkan_loss
    test_vulkan_effects
    test_vulkan_video_vulkan
    PROPERTIES
      ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
      SKIP_REGULAR_EXPRESSION "SKIPPED"
  )
  # Runs the libobs graphics API (gs_create), so it needs the libobs runtime on PATH.
  set_tests_properties(test_vulkan_yuv_compute PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
  if(OS_WINDOWS)
    set_tests_properties(
      test_vulkan_interop
      PROPERTIES
        ENVIRONMENT "OBS_VULKAN_VALIDATION=1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true"
        SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
  endif()

  # GPU (texture) encoding through the shared frames: a real encoder module encodes a moving, numbered pattern and
  # libavcodec decodes it back. Each test picks its GPU with VK_LOADER_DRIVERS_SELECT and skips without it.
  if(OS_WINDOWS AND (TARGET knurbs-nvenc OR TARGET knurbs-amf))
    find_package(FFmpeg 8.0 REQUIRED COMPONENTS avcodec avutil)
    add_executable(test_vulkan_gpu_encode test_vulkan_gpu_encode.cpp)
    target_include_directories(test_vulkan_gpu_encode PRIVATE ${CMOCKA_INCLUDE_DIR})
    target_compile_definitions(test_vulkan_gpu_encode PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_features(test_vulkan_gpu_encode PRIVATE cxx_std_17)
    target_link_libraries(test_vulkan_gpu_encode PRIVATE OBS::libobs FFmpeg::avcodec FFmpeg::avutil cmocka::cmocka)
    add_dependencies(test_vulkan_gpu_encode libobs-vulkan)
    set(_gpu_encode_tests)
    # The encoders look for their capability probes (knurbs-nvenc-test, knurbs-amf-test) next to the executable.
    macro(_gpu_encode_probe probe)
      if(TARGET ${probe})
        add_dependencies(test_vulkan_gpu_encode ${probe})
        add_custom_command(
          TARGET test_vulkan_gpu_encode
          POST_BUILD
          COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${probe}>"
                  "$<TARGET_FILE_DIR:test_vulkan_gpu_encode>"
        )
      endif()
    endmacro()
    macro(_gpu_encode_test name driver module id format)
      add_test(
        NAME test_vulkan_gpu_encode_${name}
        COMMAND
          test_vulkan_gpu_encode "$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan" "$<TARGET_FILE:${module}>" ${id}
          ${format} ${ARGN}
      )
      set_tests_properties(test_vulkan_gpu_encode_${name} PROPERTIES ENVIRONMENT "VK_LOADER_DRIVERS_SELECT=${driver}")
      list(APPEND _gpu_encode_tests test_vulkan_gpu_encode_${name})
    endmacro()
    if(TARGET knurbs-nvenc)
      add_dependencies(test_vulkan_gpu_encode knurbs-nvenc)
      _gpu_encode_probe(knurbs-nvenc-test)
      set(_nv com.xaymar.knurbs.nvenc)
      _gpu_encode_test(nvenc_h264 *nv-vk64* knurbs-nvenc ${_nv}.h264 nv12 rate_control=CQP cqp=20 bf=2)
      _gpu_encode_test(nvenc_hevc_p010 *nv-vk64* knurbs-nvenc ${_nv}.hevc p010 rate_control=CQP cqp=20 bf=2)
      # Lookahead holds many frames: the shared frame pool grows past its initial size.
      _gpu_encode_test(nvenc_h264_lookahead *nv-vk64* knurbs-nvenc ${_nv}.h264 nv12 rate_control=CQP cqp=20 bf=3 lookahead=true)
      # A stalled render: libobs repeats frames, so one shared frame sits in several NVENC slots at once.
      _gpu_encode_test(nvenc_h264_repeat *nv-vk64* knurbs-nvenc ${_nv}.h264 nv12 rate_control=CQP cqp=20 bf=2 stall=10)
    endif()
    if(TARGET knurbs-amf)
      add_dependencies(test_vulkan_gpu_encode knurbs-amf)
      _gpu_encode_probe(knurbs-amf-test)
      set(_amf com.xaymar.knurbs.amf)
      _gpu_encode_test(amf_h264 *amd-vulkan64* knurbs-amf ${_amf}.h264 nv12 rate_control=CQP cqp=20)
      _gpu_encode_test(amf_hevc_p010 *amd-vulkan64* knurbs-amf ${_amf}.hevc p010 rate_control=CQP cqp=20)
      _gpu_encode_test(amf_h264_repeat *amd-vulkan64* knurbs-amf ${_amf}.h264 nv12 rate_control=CQP cqp=20 stall=10)
      # AMF's stale frames (docs/graphics-interop.md, Driver issues): given a new AMFSurface for an image, AMF now and
      # then encodes the picture of an earlier one, so the encoder wraps each image once. With the code from before
      # that (OBS_AMF_VULKAN_TEST_WRAP_EACH_FRAME=1) the three tests above fail every run. This one covers many
      # images: the encoder keeps 76 frames held longer (OBS_AMF_VULKAN_TEST_HOLD), so libobs's frame pool grows to
      # its limit of 80 and two thirds of the frames come in images AMF has not seen yet (4 of 5 runs fail without
      # the fix).
      _gpu_encode_test(amf_h264_fresh_images *amd-vulkan64* knurbs-amf ${_amf}.h264 nv12 rate_control=CQP cqp=20)
      set_property(TEST test_vulkan_gpu_encode_amf_h264_fresh_images APPEND PROPERTY ENVIRONMENT "OBS_AMF_VULKAN_TEST_HOLD=76")
    endif()
    set_tests_properties(
      ${_gpu_encode_tests}
      PROPERTIES
        ENVIRONMENT_MODIFICATION
          "OBS_VULKAN_VALIDATION=set:1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=set:true"
        SKIP_REGULAR_EXPRESSION "SKIPPED"
    )
  endif()

  # Game capture and window capture on the Vulkan renderer: a producer window (the test itself) with a known
  # pattern, captured through the real pipeline (hook injection, WGC, BitBlt) and read back. Game capture runs against the
  # hook Knurbs ships: OBS Studio's signed release binaries (plugins/win-capture/cmake/obs-hook.cmake).
  if(OS_WINDOWS AND TARGET win-capture)
    # Installing the hook into C:\ProgramData\obs-studio-hook: signed hooks only, never a downgrade.
    obs_add_unit_test(test_win_capture_hook_install)
    target_sources(
      test_win_capture_hook_install
      PRIVATE "${CMAKE_SOURCE_DIR}/plugins/win-capture/game-capture-hook-install.c"
    )
    target_include_directories(test_win_capture_hook_install PRIVATE "${CMAKE_SOURCE_DIR}/plugins/win-capture")
    target_link_libraries(test_win_capture_hook_install PRIVATE crypt32 wintrust)
    target_compile_definitions(test_win_capture_hook_install PRIVATE OBS_SIGNED_HOOK_DIR="${KNURBS_OBS_HOOK_DIR}")
    set_tests_properties(test_win_capture_hook_install PROPERTIES SKIP_REGULAR_EXPRESSION "SKIPPED")

    add_executable(
      test_vulkan_win_capture
      test_vulkan_win_capture.cpp
      "${CMAKE_SOURCE_DIR}/plugins/win-capture/game-capture-hook-install.c"
    )
    target_include_directories(test_vulkan_win_capture PRIVATE "${CMAKE_SOURCE_DIR}/plugins/win-capture")
    target_include_directories(test_vulkan_win_capture PRIVATE ${CMOCKA_INCLUDE_DIR})
    target_compile_definitions(test_vulkan_win_capture PRIVATE OBS_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_compile_features(test_vulkan_win_capture PRIVATE cxx_std_17)
    target_link_libraries(test_vulkan_win_capture PRIVATE OBS::libobs d3d11 dxgi cmocka::cmocka crypt32 wintrust)
    add_dependencies(test_vulkan_win_capture libobs-vulkan win-capture libobs-winrt)
    set(_win_capture_dir "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_PLUGIN_DESTINATION}/win-capture")
    set(_win_capture_data "${_win_capture_dir}/data")
    set(_win_capture_tests)
    macro(_win_capture_test name driver data case)
      set(_module "$<TARGET_FILE_DIR:libobs-vulkan>/libobs-vulkan")
      if("${driver}" STREQUAL "d3d11")
        set(_module "$<TARGET_FILE_DIR:libobs-d3d11>/libobs-d3d11")
      endif()
      add_test(
        NAME test_vulkan_win_capture_${name}
        COMMAND test_vulkan_win_capture "${_module}" "${_win_capture_dir}/win-capture.dll" "${data}" ${case}
      )
      set_tests_properties(test_vulkan_win_capture_${name} PROPERTIES ENVIRONMENT "VK_LOADER_DRIVERS_SELECT=${driver}")
      list(APPEND _win_capture_tests test_vulkan_win_capture_${name})
    endmacro()
    # d3d11: the same cases on the D3D11 renderer, a reference for the paths Vulkan must match.
    set(_gpus nv amd)
    if(TARGET libobs-d3d11)
      add_dependencies(test_vulkan_win_capture libobs-d3d11)
      list(APPEND _gpus d3d11)
    endif()
    foreach(_gpu IN LISTS _gpus)
      set(_driver "*${_gpu}-vk64*")
      if(_gpu STREQUAL "amd")
        set(_driver "*amd-vulkan64*")
      elseif(_gpu STREQUAL "d3d11")
        set(_driver "d3d11")
      endif()
      foreach(
        _case
        IN ITEMS
          game_bgra
          game_rgba
          game_rgb10a2
          game_rgba16f
          game_shmem_bgra
          game_bgra_loss
          wgc_d3d11
          wgc_d3d11_hdr
          wgc_d3d11_loss
          wgc_gdi
          wgc_gdi_resize
          bitblt_gdi
          display_dxgi
          display_dxgi_hdr
          display_dxgi_loss
          display_dxgi_mode
          display_wgc
          display_wgc_hdr
          display_wgc_loss
          display_wgc_mode
      )
        if(NOT (_gpu STREQUAL "d3d11" AND _case MATCHES "_loss$|^display_dxgi_hdr$")) # simulated by libobs-vulkan / the duplicator
          _win_capture_test(${_gpu}_${_case} ${_driver} "${_win_capture_data}" ${_case})
        endif()
      endforeach()
    endforeach()
    set_tests_properties(
      ${_win_capture_tests}
      PROPERTIES
        ENVIRONMENT_MODIFICATION
          "OBS_VULKAN_VALIDATION=set:1;VK_KHRONOS_VALIDATION_VALIDATE_SYNC=set:true;${_obs_runtime_env}"
        SKIP_REGULAR_EXPRESSION "SKIPPED"
        RUN_SERIAL TRUE
    )
  endif()
endif()

# OS path test
add_executable(test_os_path test_os_path.c)
target_include_directories(test_os_path PRIVATE ${CMOCKA_INCLUDE_DIR})
target_link_libraries(test_os_path PRIVATE OBS::libobs cmocka::cmocka)

add_test(NAME test_os_path COMMAND test_os_path)

set_tests_properties(
  test_serializer
  test_darray
  test_bitstream
  test_os_path
  PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}"
)

# Tests that start libobs: run from the rundir so libobs finds its data files (../../data/libobs)
if(TARGET libobs-null)
  # Canvas weak reference regression test
  add_executable(test_obs_canvas_weak test_obs_canvas_weak.c)
  target_include_directories(test_obs_canvas_weak PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_obs_canvas_weak PRIVATE OBS::libobs cmocka::cmocka)
  add_test(
    NAME test_obs_canvas_weak
    COMMAND test_obs_canvas_weak
    WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  )

  # Video thread on the null graphics module
  add_executable(test_obs_video_null test_obs_video_null.c)
  target_include_directories(test_obs_video_null PRIVATE ${CMOCKA_INCLUDE_DIR})
  target_link_libraries(test_obs_video_null PRIVATE OBS::libobs cmocka::cmocka)
  target_compile_definitions(test_obs_video_null PRIVATE NULL_GRAPHICS_MODULE="$<TARGET_FILE:libobs-null>")
  add_dependencies(test_obs_video_null libobs-null)
  add_test(
    NAME test_obs_video_null
    COMMAND test_obs_video_null
    WORKING_DIRECTORY "${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}"
  )

  set_tests_properties(test_obs_canvas_weak test_obs_video_null PROPERTIES ENVIRONMENT_MODIFICATION "${_obs_runtime_env}")
endif()

foreach(test IN ITEMS test_serializer test_darray test_bitstream test_os_path test_obs_canvas_weak test_obs_video_null)
  if(TARGET ${test})
    add_dependencies(obs-unit-tests ${test})
  endif()
endforeach()
if(TARGET test-obs-mem)
  add_dependencies(obs-unit-tests test-obs-mem)
endif()

# util/
foreach(
  test
  IN
  ITEMS
    test_util_base
    test_util_bmem
    test_util_crc32
    test_util_darray
    test_util_deque
    test_util_dstr
    test_util_lexer
    test_util_cf_lexer
    test_util_cf_parser
    test_util_config_file
    test_util_text_lookup
    test_util_array_serializer
    test_util_file_serializer
    test_util_buffered_file_serializer
    test_util_platform
    test_util_pipe
    test_util_profiler
    test_util_threading
    test_util_task
    test_util_spsc_fifo
)
  obs_add_unit_test(${test})
endforeach()
if(OS_WINDOWS)
  obs_add_unit_test(test_util_windows)
endif()

# media-io/
foreach(
  test
  IN
  ITEMS
    test_media_format_conversion
    test_media_video_frame
    test_media_video_formats
    test_media_audio_resampler
    test_media_video_scaler
    test_media_video_io
    test_media_audio_io
    test_media_remux
)
  obs_add_unit_test(${test})
endforeach()

# graphics/ math
foreach(
  test
  IN
  ITEMS
    test_graphics_vec
    test_graphics_matrix
    test_graphics_geometry
    test_graphics_shader_parser
)
  obs_add_unit_test(${test})
endforeach()

# graphics/
foreach(
  test
  IN
  ITEMS
    test_graphics_graphics
    test_graphics_failures
    test_graphics_effect
    test_graphics_image_file
    test_graphics_texture_render
)
  obs_add_graphics_test(${test})
endforeach()

# core/
foreach(
  test
  IN
  ITEMS
    test_obs_data
    test_obs_properties
    test_callback
    test_obs_codec_helpers
)
  obs_add_graphics_test(${test})
endforeach()

# core/ libobs
foreach(
  test
  IN
  ITEMS
    test_obs_source
    test_obs_source_av
    test_obs_source_profiler
    test_obs_register
    test_obs_video_formats
    test_obs_video_failures
    test_obs_scene
    test_obs_output
    test_obs_encoder_slow
    test_obs_audio
    test_obs_hotkey
    test_obs_canvas
)
  obs_add_core_test(${test})
endforeach()

# fake module for test_obs_module
add_library(fake-module MODULE fake-module/fake-module.c)
target_link_libraries(fake-module PRIVATE OBS::libobs)
set_target_properties(fake-module PROPERTIES FOLDER "Tests and Examples" PREFIX "")
obs_add_core_test(test_obs_module)
target_compile_definitions(
  test_obs_module
  PRIVATE FAKE_MODULE_PATH="$<TARGET_FILE:fake-module>" FAKE_MODULE_DATA="${CMAKE_CURRENT_SOURCE_DIR}/fake-module/data"
)
add_dependencies(test_obs_module fake-module)

# Every test program reports failures on stderr instead of opening a dialog (see noninteractive.c), and no
# test may hang the run.
get_property(_test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_target IN LISTS _test_targets)
  get_target_property(_type ${_target} TYPE)
  if(_type STREQUAL "EXECUTABLE")
    target_sources(${_target} PRIVATE "${CMAKE_SOURCE_DIR}/test/common/noninteractive.c")
  endif()
endforeach()
get_property(_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_tests} PROPERTIES TIMEOUT 300)
