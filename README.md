# vsdmars_lib

A modern **C++23** high-performance utility and container library modeled after [Meta's Folly](https://github.com/facebook/folly/tree/main/folly).

## Layout

```text
vsdmars_lib/
├── CMakeLists.txt
├── cmake/vsdmars_libConfig.cmake.in
├── .clangd
├── .clang-tidy
└── vsdmars/
    └── container/
        ├── b_heap.h            # Cache-friendly SoA B-Heap (vsdmars::BHeap)
        └── test/
            └── b_heap_test.cpp
```

## Build & Run Tests

Unit tests use **GoogleTest** (discovered via `find_package(GTest)` or fetched automatically via `FetchContent`).

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

To also run `clang-tidy` (including all `.h` headers under `vsdmars/`) during the build:

```bash
cmake -S . -B build -G Ninja -DVSDMARS_ENABLE_CLANG_TIDY=ON
cmake --build build
```

## Using in Another CMake Project

### Option 1: `add_subdirectory`

```cmake
add_subdirectory(/path/to/vsdmars_lib vsdmars_lib)
target_link_libraries(my_app PRIVATE vsdmars::vsdmars_lib)
```

### Option 2: `FetchContent`

```cmake
include(FetchContent)
FetchContent_Declare(
  vsdmars_lib
  SOURCE_DIR /path/to/vsdmars_lib # or GIT_REPOSITORY / GIT_TAG
)
FetchContent_MakeAvailable(vsdmars_lib)
target_link_libraries(my_app PRIVATE vsdmars::vsdmars_lib)
```

### Option 3: Install & `find_package`

```bash
cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --install build
```

```cmake
find_package(vsdmars_lib CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE vsdmars::vsdmars_lib)
```

### C++ Example

```cpp
#include <vsdmars/container/b_heap.h>

vsdmars::BHeap<int, std::string, std::greater<>> pq;
pq.push(10, "task-a");
pq.push(5, "task-b");
pq.reschedule_top(20); // updates root priority in-place
```
