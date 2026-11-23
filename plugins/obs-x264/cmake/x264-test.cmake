add_executable(obs-x264-test)

target_sources(obs-x264-test PRIVATE obs-x264-test.c)

target_compile_options(obs-x264-test PRIVATE $<$<COMPILE_LANG_AND_ID:C,AppleClang,Clang>:-Wno-strict-prototypes>)

target_link_libraries(obs-x264-test PRIVATE OBS::opts-parser)

add_test(NAME obs-x264-test COMMAND obs-x264-test)
if(OS_WINDOWS)
  # obs.dll, w32-pthreads.dll and the obs-deps libraries are not next to the test executable on Windows
  set(_x264_test_env "PATH=path_list_prepend:${OBS_OUTPUT_DIR}/$<CONFIG>/${OBS_EXECUTABLE_DESTINATION}")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND _x264_test_env "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
  set_tests_properties(obs-x264-test PROPERTIES ENVIRONMENT_MODIFICATION "${_x264_test_env}" TIMEOUT 300)
endif()

set_target_properties(obs-x264-test PROPERTIES FOLDER plugins/obs-x264)
