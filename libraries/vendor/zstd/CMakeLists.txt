# zstd 1.5.5 (BSD-3-Clause / GPLv2, see LICENSE), vendored so block_log compression does not depend on the
# zstd version of the build OS. Only the library sources are included (no CLI, no legacy/deprecated code).
cmake_minimum_required( VERSION 3.5 )

file( GLOB ZSTD_SOURCES common/*.c compress/*.c decompress/*.c dictBuilder/*.c )

add_library( zstd_static STATIC ${ZSTD_SOURCES} )
target_include_directories( zstd_static PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" )
# no hand written assembly in the Huffman decoder: keeps the build portable
target_compile_definitions( zstd_static PRIVATE ZSTD_DISABLE_ASM=1 )
set_target_properties( zstd_static PROPERTIES POSITION_INDEPENDENT_CODE ON C_STANDARD 99 )
