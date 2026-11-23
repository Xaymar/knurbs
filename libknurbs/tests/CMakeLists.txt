# Unit tests for libknurbs. Included from libknurbs/CMakeLists.txt.

if(NOT TARGET cmocka::cmocka)
  find_package(CMocka CONFIG QUIET)
  if(NOT TARGET cmocka::cmocka)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CMOCKA REQUIRED IMPORTED_TARGET cmocka)
    add_library(cmocka::cmocka INTERFACE IMPORTED)
    target_link_libraries(cmocka::cmocka INTERFACE PkgConfig::CMOCKA)
  endif()
endif()

# knurbs.dll and the dependency runtime libraries are not next to the test executables on Windows
set(_knurbs_runtime_env "")
if(WIN32)
  list(APPEND _knurbs_runtime_env "PATH=path_list_prepend:$<TARGET_FILE_DIR:libknurbs>")
  foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
    if(EXISTS "${prefix}/bin")
      list(APPEND _knurbs_runtime_env "PATH=path_list_prepend:${prefix}/bin")
    endif()
  endforeach()
endif()

add_custom_target(knurbs-tests)

# knurbs_add_test(<name> [C]): tests/<name>.cpp, or tests/c/<name>.c (C11) with C, linked with libknurbs and cmocka
function(knurbs_add_test name)
  if("C" IN_LIST ARGN)
    add_executable(${name} c/${name}.c)
    set_target_properties(
      ${name}
      PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
    )
    # The C headers must stay clean C11
    if(NOT MSVC)
      target_compile_options(${name} PRIVATE -Wall -Wextra -pedantic)
    endif()
  else()
    add_executable(${name} ${name}.cpp)
  endif()
  target_compile_definitions(${name} PRIVATE KNURBS_CHECK_KEYS)
  # Keep char8_t on when OBS turns it off (see libknurbs/CMakeLists.txt)
  target_compile_options(
    ${name}
    PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fchar8_t> $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:char8_t>
  )
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${name} PRIVATE knurbs::knurbs cmocka::cmocka)
  set_target_properties(${name} PROPERTIES FOLDER "Tests and Examples")
  set(work_dir "${CMAKE_CURRENT_BINARY_DIR}/work/${name}")
  file(MAKE_DIRECTORY "${work_dir}")
  add_test(NAME ${name} COMMAND ${name} WORKING_DIRECTORY "${work_dir}")
  if(_knurbs_runtime_env)
    set_tests_properties(${name} PROPERTIES ENVIRONMENT_MODIFICATION "${_knurbs_runtime_env}")
  endif()
  add_dependencies(knurbs-tests ${name})
endfunction()

knurbs_add_test(test_version)
knurbs_add_test(test_error)
knurbs_add_test(test_object)
knurbs_add_test(test_function)
knurbs_add_test(test_log)
knurbs_add_test(test_thread)
knurbs_add_test(test_platform)
knurbs_add_test(test_naming)
knurbs_add_test(test_core)
knurbs_add_test(test_data)
knurbs_add_test(test_properties)
knurbs_add_test(test_registry)
knurbs_add_test(test_locale)
# The test data is addressed by absolute path, not relative to the working directory
target_compile_definitions(test_locale PRIVATE KNURBS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# Fake modules for test_module, laid out like real ones: modules/<name>/bin/<prefix><name><suffix> and an empty
# modules/<name>/data/. The loader expects CMake's MODULE prefix and suffix (see libknurbs/CMakeLists.txt).
set(_knurbs_test_modules_dir "${CMAKE_CURRENT_BINARY_DIR}/modules")

# knurbs_add_fake_module(<name> <source> [<modules dir>]): tests/modules/<source> (C++, or C11 for a .c file), linked
# with libknurbs, in <modules dir> (default: the one test_module loads)
function(knurbs_add_fake_module name source)
  set(modules_dir "${_knurbs_test_modules_dir}")
  if(ARGC GREATER 2)
    set(modules_dir "${ARGV2}")
  endif()
  add_library(${name} MODULE modules/${source})
  # The C headers must stay clean C11
  if(source MATCHES "\\.c$" AND NOT MSVC)
    target_compile_options(${name} PRIVATE -Wall -Wextra -pedantic)
  endif()
  # Keep char8_t on when OBS turns it off (see libknurbs/CMakeLists.txt)
  target_compile_options(
    ${name}
    PRIVATE $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fchar8_t> $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:/Zc:char8_t>
  )
  target_link_libraries(${name} PRIVATE knurbs::knurbs)
  # The generator expression keeps multi-config generators from appending a per-configuration directory.
  set_target_properties(
    ${name}
    PROPERTIES
      FOLDER "Tests and Examples"
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      C_VISIBILITY_PRESET hidden
      CXX_VISIBILITY_PRESET hidden
      VISIBILITY_INLINES_HIDDEN ON
      LIBRARY_OUTPUT_DIRECTORY "$<1:${modules_dir}/${name}/bin>"
      RUNTIME_OUTPUT_DIRECTORY "$<1:${modules_dir}/${name}/bin>"
  )
  file(MAKE_DIRECTORY "${modules_dir}/${name}/data")
  add_dependencies(knurbs-tests ${name})
  set_property(GLOBAL APPEND PROPERTY _knurbs_fake_modules ${name})
endfunction()

knurbs_add_fake_module(fake-c fake-c.c)
knurbs_add_fake_module(fake-ok fake-ok.cpp)
knurbs_add_fake_module(fake-throws fake-throws.cpp)
knurbs_add_fake_module(fake-other-major fake-other-major.cpp)
# fake-c for another API major version, kept out of the directory above
set(_knurbs_test_other_modules_dir "${CMAKE_CURRENT_BINARY_DIR}/other-modules")
knurbs_add_fake_module(fake-c-other-major fake-c.c "${_knurbs_test_other_modules_dir}")
target_compile_definitions(fake-c-other-major PRIVATE FAKE_C_API_MAJOR=99)

knurbs_add_test(test_module)
get_property(_knurbs_fake_modules GLOBAL PROPERTY _knurbs_fake_modules)
add_dependencies(test_module ${_knurbs_fake_modules})
target_compile_definitions(
  test_module
  PRIVATE
    KNURBS_TEST_MODULES_DIR="${_knurbs_test_modules_dir}"
    KNURBS_TEST_OTHER_MODULES_DIR="${_knurbs_test_other_modules_dir}"
)

# C11 with -pedantic: includes the C headers for objects, core, logging and modules
knurbs_add_test(test_c_object C)

# C11 with -pedantic: includes the C headers for data and properties
knurbs_add_test(test_c_data C)

# C11 with -pedantic: includes the C headers for the registry and modules, and loads the fake modules
knurbs_add_test(test_c_registry C)
add_dependencies(test_c_registry ${_knurbs_fake_modules})
target_compile_definitions(test_c_registry PRIVATE KNURBS_TEST_MODULES_DIR="${_knurbs_test_modules_dir}")

knurbs_apply_macos_minimum("${CMAKE_CURRENT_SOURCE_DIR}")
