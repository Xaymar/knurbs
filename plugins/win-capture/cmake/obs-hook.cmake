# OBS Studio's official game capture hook
#
# Knurbs does not build graphics-hook, inject-helper and get-graphics-offsets: it ships the ones from an OBS Studio
# release, unchanged and Authenticode-signed by OBS Project, LLC (game-capture-file-init.c only installs a signed hook
# into C:\ProgramData\obs-studio-hook). The release archive and every file taken from it are pinned by SHA-256.
#
# Bumping the pin (see docs/game-capture-hook.md): pick the newest stable OBS Studio release whose
# shared/obs-hook-config (graphics-hook-info.h, graphics-hook-ver.h, hook-helpers.h) matches ours, update
# KNURBS_OBS_HOOK_VERSION and the archive hash (GitHub lists it as the asset's digest), delete the old .deps/obs-hook-*
# directory and configure once with KNURBS_OBS_HOOK_PRINT_HASHES=ON to print the new file hashes, check every exe/dll
# with Get-AuthenticodeSignature, and paste the hashes below.
#
# Offline builds: set KNURBS_OBS_HOOK_ARCHIVE to a local copy of the release archive.

include_guard(GLOBAL)

set(KNURBS_OBS_HOOK_VERSION 33.0.3)
set(_obs_hook_archive_name "OBS-Studio-${KNURBS_OBS_HOOK_VERSION}-Windows-x64.zip")
set(
  _obs_hook_archive_url
  "https://github.com/obsproject/obs-studio/releases/download/${KNURBS_OBS_HOOK_VERSION}/${_obs_hook_archive_name}"
)
set(_obs_hook_archive_sha256 18a68c325adbda6f6082d5281021a8d1e35a6432842800618e8690b71925c107)
set(_obs_hook_archive_prefix "core/win-capture/data")

# gersemi: off
set(_obs_hook_files
  graphics-hook32.dll          e51b81645df3b06458547d9433eaa6b679b1e8feb214ec14ecdfeb23e2a34b66
  graphics-hook64.dll          1261cf476829354a3831e97bc77b9af6a1340832e3ed4e079dc2b98b79857d2a
  inject-helper32.exe          2bcd432d538ef4a9999ff9be5be1cd9079dff94eb60c7e1ad0886bf6a822d122
  inject-helper64.exe          4ecc338f9232817f19a3f0228bd3de4fb8bddabffa65a94b9227c02a1545bb0d
  get-graphics-offsets32.exe   d761a8abfc2afb8392d561766a06a7d44d072558c49996dc78952ca221b6ad13
  get-graphics-offsets64.exe   46aa1bd9b246d5165a27b597b791b3b6d5e8bf6a1b4008f510bc9995cccebdac
  obs-vulkan32.json            c16e0707ae66ad71e8a0720aeb6e6997a1017f19762333452aef692115a9ab41
  obs-vulkan64.json            7b6b0813fb58b276847a8583eb5c3f94aee7d7ad0ae3a1ef6133d5d8771f20f4
)
# gersemi: on

set(KNURBS_OBS_HOOK_ARCHIVE "" CACHE FILEPATH "Local copy of ${_obs_hook_archive_name} (otherwise downloaded)")
option(KNURBS_OBS_HOOK_PRINT_HASHES "Print the SHA-256 of the extracted hook files (for bumping the pin)" OFF)
mark_as_advanced(KNURBS_OBS_HOOK_ARCHIVE KNURBS_OBS_HOOK_PRINT_HASHES)

set(_obs_hook_dir "${CMAKE_SOURCE_DIR}/.deps/obs-hook-${KNURBS_OBS_HOOK_VERSION}")

# True when every pinned file is present in the directory with its pinned hash.
function(_obs_hook_verify dir result)
  set(_ok TRUE)
  set(_list ${_obs_hook_files})
  while(_list)
    list(POP_FRONT _list _name _hash)
    if(NOT EXISTS "${dir}/${_name}")
      set(_ok FALSE)
      break()
    endif()
    file(SHA256 "${dir}/${_name}" _actual)
    if(KNURBS_OBS_HOOK_PRINT_HASHES)
      message(STATUS "OBS hook: ${_name} ${_actual}")
    endif()
    if(NOT _actual STREQUAL _hash)
      message(WARNING "OBS hook: ${dir}/${_name} has SHA-256 ${_actual}, expected ${_hash}")
      set(_ok FALSE)
    endif()
  endwhile()
  set(${result} ${_ok} PARENT_SCOPE)
endfunction()

_obs_hook_verify("${_obs_hook_dir}/data" _obs_hook_ok)
if(NOT _obs_hook_ok)
  file(REMOVE_RECURSE "${_obs_hook_dir}")
  if(KNURBS_OBS_HOOK_ARCHIVE)
    set(_archive "${KNURBS_OBS_HOOK_ARCHIVE}")
    file(SHA256 "${_archive}" _actual)
    if(NOT _actual STREQUAL _obs_hook_archive_sha256)
      message(FATAL_ERROR "OBS hook: ${_archive} has SHA-256 ${_actual}, expected ${_obs_hook_archive_sha256}")
    endif()
  else()
    set(_archive "${_obs_hook_dir}/${_obs_hook_archive_name}")
    message(STATUS "OBS hook: downloading ${_obs_hook_archive_url}")
    file(DOWNLOAD "${_obs_hook_archive_url}" "${_archive}" TLS_VERIFY ON STATUS _status)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
      file(REMOVE_RECURSE "${_obs_hook_dir}")
      message(
        FATAL_ERROR
        "OBS hook: download of ${_obs_hook_archive_url} failed: ${_status} (KNURBS_OBS_HOOK_ARCHIVE takes a local copy)"
      )
    endif()
    file(SHA256 "${_archive}" _actual)
    if(NOT _actual STREQUAL _obs_hook_archive_sha256)
      file(REMOVE_RECURSE "${_obs_hook_dir}")
      message(FATAL_ERROR "OBS hook: ${_obs_hook_archive_url} has SHA-256 ${_actual}, expected ${_obs_hook_archive_sha256}")
    endif()
  endif()

  set(_patterns)
  set(_list ${_obs_hook_files})
  while(_list)
    list(POP_FRONT _list _name _hash)
    list(APPEND _patterns "${_obs_hook_archive_prefix}/${_name}")
  endwhile()
  file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_obs_hook_dir}/extract" PATTERNS ${_patterns})
  file(COPY "${_obs_hook_dir}/extract/${_obs_hook_archive_prefix}/" DESTINATION "${_obs_hook_dir}/data")
  file(REMOVE_RECURSE "${_obs_hook_dir}/extract")
  if(NOT KNURBS_OBS_HOOK_ARCHIVE)
    file(REMOVE "${_archive}")
  endif()

  _obs_hook_verify("${_obs_hook_dir}/data" _obs_hook_ok)
  if(NOT _obs_hook_ok)
    message(FATAL_ERROR "OBS hook: the files extracted from ${_obs_hook_archive_name} do not match the pinned hashes")
  endif()
endif()

message(STATUS "OBS hook: OBS Studio ${KNURBS_OBS_HOOK_VERSION} signed hook binaries in ${_obs_hook_dir}/data")

# The hook binaries' directory, for the tests.
set(KNURBS_OBS_HOOK_DIR "${_obs_hook_dir}/data" CACHE INTERNAL "OBS Studio's signed hook binaries")

# obs_hook_add_resources(<target>): ships the pinned hook files with <target> in win-capture's data directory.
function(obs_hook_add_resources target)
  set(_list ${_obs_hook_files})
  while(_list)
    list(POP_FRONT _list _name _hash)
    target_add_resource(${target} "${KNURBS_OBS_HOOK_DIR}/${_name}" "${OBS_PLUGIN_DESTINATION}/win-capture/data")
  endwhile()
endfunction()
