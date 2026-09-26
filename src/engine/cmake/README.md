# `src/engine/cmake/`

Engine modules add their sources to the plugin from here instead of editing the shared
`src/engine/CMakeLists.txt`. The root `CMakeLists.txt` includes every `.cmake` file in this folder.

```cmake
# src/engine/cmake/<name>.cmake
target_sources(echoreplay PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../wiretap.cpp)
```

`${CMAKE_CURRENT_LIST_DIR}` is this folder while the file is being included, so name sources relative
to it.

Files are included in name order, which is why they carry a number prefix: it keeps the link order
stable.

A few `target_sources` lines in `src/engine/CMakeLists.txt` predate this folder. They work as they are.
