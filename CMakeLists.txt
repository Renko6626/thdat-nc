cmake_minimum_required(VERSION 3.16)
project(thdat-nc VERSION 0.2.0 LANGUAGES C)

add_executable(thdat-nc
  src/main.c
  src/common.c
  src/pkgl.c
  src/zstd_api.c
  src/platform.c
  src/unpack.c
  src/pack.c
)
target_compile_features(thdat-nc PRIVATE c_std_11)
# Feature macros live here, not in the sources, so every file sees the same ones.
target_compile_definitions(thdat-nc PRIVATE _FILE_OFFSET_BITS=64)

if(MSVC)
  target_compile_options(thdat-nc PRIVATE /W4)
else()
  target_compile_options(thdat-nc PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
  find_path(ZSTD_INCLUDE_DIR zstd.h REQUIRED)
  find_library(ZSTD_LIBRARY NAMES zstd libzstd REQUIRED)
  target_include_directories(thdat-nc PRIVATE "${ZSTD_INCLUDE_DIR}")
  target_link_libraries(thdat-nc PRIVATE "${ZSTD_LIBRARY}")
else()
  target_compile_definitions(thdat-nc PRIVATE _POSIX_C_SOURCE=200809L)
  target_link_libraries(thdat-nc PRIVATE "${CMAKE_DL_LIBS}")
endif()

install(TARGETS thdat-nc RUNTIME DESTINATION bin)
