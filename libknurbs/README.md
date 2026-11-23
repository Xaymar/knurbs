# libknurbs

The C++23 core library of Knurbs. It is built beside libobs and ships as a shared library with a C++ API
(`knurbs/*.hpp`) and a C API (`knurbs/c/*.h`).

## Toolchain floors

| Toolchain  | Minimum                                                                   |
| ---------- | ------------------------------------------------------------------------- |
| MSVC       | 17.14                                                                     |
| GCC        | 14 (libstdc++)                                                            |
| AppleClang | to be confirmed (libc++); macOS deployment target to be decided with the first macOS build |

## C++23 library features in use

| Feature | Header | Used in |
| ------- | ------ | ------- |
| `std::move_only_function` | `<functional>` | `knurbs/log.hpp` (log sinks), `knurbs/thread.hpp` (queued tasks) |
| `std::format`, `std::format_string` | `<format>` | `knurbs/log.hpp` (logger formatting helpers) |

## Building standalone

```sh
cmake -S libknurbs -B build-knurbs -G Ninja
cmake --build build-knurbs
ctest --test-dir build-knurbs --output-on-failure
```
