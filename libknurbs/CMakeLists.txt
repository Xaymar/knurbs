cmake_minimum_required(VERSION 3.28...3.30)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  # Standalone build: cmake -S libknurbs -B build-knurbs
  project(knurbs VERSION 0.1.0 LANGUAGES C CXX)
  enable_testing()
  option(KNURBS_BUILD_TESTS "Build the libknurbs unit tests" ON)
else()
  # Part of the OBS build: tests follow ENABLE_UNIT_TESTS
  if(ENABLE_UNIT_TESTS)
    set(KNURBS_BUILD_TESTS ON)
  else()
    set(KNURBS_BUILD_TESTS OFF)
  endif()
endif()

include(GNUInstallDirs)

add_library(libknurbs SHARED)
add_library(knurbs::knurbs ALIAS libknurbs)

target_sources(libknurbs PRIVATE source/error.cpp source/object.cpp source/version.cpp)

target_include_directories(
  libknurbs
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(libknurbs PUBLIC cxx_std_23)
target_compile_definitions(libknurbs PRIVATE KNURBS_BUILDING)

if(MSVC)
  target_compile_options(libknurbs PUBLIC /EHsc /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(libknurbs PRIVATE -Wall -Wextra)
endif()

set_target_properties(
  libknurbs
  PROPERTIES OUTPUT_NAME knurbs VERSION 0.1.0 SOVERSION 0 CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON
)

install(DIRECTORY include/knurbs DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(
  TARGETS libknurbs
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

if(KNURBS_BUILD_TESTS)
  add_subdirectory(tests)
endif()
