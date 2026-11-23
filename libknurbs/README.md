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
no tests) since the final review, and its first green run decides both rows. Until then:

- The deployment target is OBS's, `CMAKE_OSX_DEPLOYMENT_TARGET` 13.0 (`CMakePresets.json`, passed to Xcode by
  `cmake/macos/xcode.cmake`). libknurbs does not set its own.
- Risks the first build answers: `std::move_only_function` in libc++ (used in public headers; if it is missing, a
  `knurbs::detail` replacement must be one type on every platform), `std::jthread` (experimental in libc++ before
  LLVM 18), and libc++'s availability markup for `std::format` (floating-point `std::to_chars` needs macOS 13.3,
  which may force the deployment target above 13.0).

The C++ API passes standard library types (`std::string`, `std::vector`, `std::filesystem::path`,
`std::move_only_function`), so a C++ plugin must be built with the same toolchain and, on Windows, the same
configuration as libknurbs: a Debug plugin against a Release `knurbs.dll` (another `_ITERATOR_DEBUG_LEVEL`) breaks
silently. The C API has no such restriction.

## C++23 library features in use

| Feature | Header | Used in |
| ------- | ------ | ------- |
| `std::move_only_function` | `<functional>` | `knurbs/log.hpp` (log sinks), `knurbs/thread.hpp` (queued tasks), `knurbs/properties.hpp` (button and modified callbacks) |
| `std::format`, `std::format_string` | `<format>` | `knurbs/log.hpp` (logger formatting helpers) |
| `std::jthread` (C++20; listed for libc++, where it was experimental before LLVM 18) | `<thread>` | `source/thread.cpp` (task queue thread), `source/module-loader.cpp` (entry-point threads) |

## Building standalone

```sh
cmake -S libknurbs -B build-knurbs -G Ninja
cmake --build build-knurbs
ctest --test-dir build-knurbs --output-on-failure
```
