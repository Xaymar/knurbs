# libknurbs

The C++23 core library of Knurbs. It is built beside libobs and ships as a shared library with a C++ API
(`knurbs/*.hpp`) and a C API (`knurbs/c/*.h`).

## Toolchain floors

| Toolchain  | Minimum                                                                   |
| ---------- | ------------------------------------------------------------------------- |
| MSVC       | 17.14                                                                     |
| GCC        | 14 (libstdc++)                                                            |
| AppleClang | TODO: not yet known (libc++). See below.                                  |

TODO (AppleClang floor and macOS deployment target): the macOS CI job builds the `libknurbs` target (compile only,
no tests) since the final review; its first green run decides both rows. What is known:

- The deployment target is OBS's, `CMAKE_OSX_DEPLOYMENT_TARGET` 13.0 (`CMakePresets.json`, passed to Xcode by
  `cmake/macos/xcode.cmake`). libknurbs does not set its own.
- Apple's libc++ in Xcode 26.5 has no `std::move_only_function` (macOS CI on 22b9fe316), so libknurbs uses its own
  `knurbs::move_only_function` (`knurbs/function.hpp`) on every platform. The same CI build reported no error for
  `std::jthread`, `std::format` or the other features below.
- Still to confirm by a green macOS build: libc++'s availability markup for `std::format` (floating-point
  `std::to_chars` needs macOS 13.3, which may force the deployment target above 13.0).

The C++ API passes standard library types (`std::string`, `std::vector`, `std::filesystem::path`,
`knurbs::move_only_function`), so a C++ plugin must be built with the same toolchain and, on Windows, the same
configuration as libknurbs: a Debug plugin against a Release `knurbs.dll` (another `_ITERATOR_DEBUG_LEVEL`) breaks
silently. The C API has no such restriction.

## C++23 library features in use

Library features newer than C++17, with their state in libc++ (macOS). The libc++ column is what macOS CI (Xcode
26.5) and a local clang 18 build with `-stdlib=libc++` showed.

| Feature | Header | Used in | libc++ |
| ------- | ------ | ------- | ------ |
| `std::move_only_function` | `<functional>` | Not used: missing from libc++. `knurbs::move_only_function` (`knurbs/function.hpp`) replaces it everywhere: log sinks, queued tasks, button and modified callbacks | Missing (Xcode 26.5, LLVM 18) |
| `std::format`, `std::format_string` | `<format>` | `knurbs/log.hpp` (logger formatting helpers), error messages in `source/` | Present (LLVM 17 and later); availability on macOS < 13.3 to confirm |
| `std::jthread` (C++20) | `<thread>` | `source/thread.cpp` (task queue thread), `source/module-loader.cpp` (entry-point threads); not in public headers | Present in Xcode 26.5; experimental (`-fexperimental-library`) in LLVM 18 |
| `std::barrier` (C++20) | `<barrier>` | `tests/` only | Present (LLVM 11 and later; macOS 11) |

## Building standalone

```sh
cmake -S libknurbs -B build-knurbs -G Ninja
cmake --build build-knurbs
ctest --test-dir build-knurbs --output-on-failure
```
