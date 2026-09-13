cmake_minimum_required(VERSION 3.16)
project(yfs_fuse C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_compile_options(-Wall -Wextra -pthread -g -std=c2x)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${FUSE3_INCLUDE_DIRS}
)

link_directories(
    ${FUSE3_LIBRARY_DIRS}
    /usr/local/lib
)

# Core yFS C library
add_library(yfs_core_lib STATIC
    src/block_dev.c
    src/buffer_cache.c
    src/journal.c
    src/tx_manager.c
    src/yfs_core.c
)
target_link_libraries(yfs_core_lib pthread)

# Formatter tool (newfs_yfs)
add_executable(newfs_yfs src/newfs_yfs.c)
target_link_libraries(newfs_yfs yfs_core_lib pthread)

# FUSE v3 daemon
add_executable(yfs_fuse src/yfs_fuse.c)
target_compile_definitions(yfs_fuse PRIVATE FUSE_USE_VERSION=31)
target_link_libraries(yfs_fuse yfs_core_lib ${FUSE3_LIBRARIES} pthread)

# Test suite
add_executable(yfs_test tests/yfs_test.c)
target_link_libraries(yfs_test yfs_core_lib pthread)
