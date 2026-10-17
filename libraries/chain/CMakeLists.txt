file(GLOB HEADERS "include/steem/chain/*.hpp" "include/steem/chain/util/*.hpp" "include/steem/chain/smt_objects/*.hpp" "include/steem/chain/sps_objects/*.hpp")

## SORT .cpp by most likely to change / break compile
add_library( steem_chain

             # As database takes the longest to compile, start it first
             database.cpp
             index.cpp

             smt_evaluator.cpp
             sps_evaluator.cpp

             smt_objects/smt_market_maker.cpp
             smt_objects/nai_pool.cpp
             steem_evaluator.cpp
             steem_objects.cpp
             required_action_evaluator.cpp
             optional_action_evaluator.cpp

             witness_schedule.cpp
             fork_database.cpp

             shared_authority.cpp
             block_log.cpp
             block_log_compression.cpp
             block_log_dictionaries.cpp

             generic_custom_operation_interpreter.cpp

             util/rd.cpp
             util/reward.cpp
             util/impacted.cpp
             util/advanced_benchmark_dumper.cpp
             util/smt_token.cpp
             util/sps_processor.cpp
             util/sps_helper.cpp

             ${HEADERS}
           )

# The built-in zstd dictionaries for the block log (block_log_dictionaries/*.bin) become a C++ table at build time.
find_package( PythonInterp 3 REQUIRED )
set( BLOCK_LOG_DICT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/block_log_dictionaries" )
file( GLOB BLOCK_LOG_DICT_FILES "${BLOCK_LOG_DICT_DIR}/*.bin" )
add_custom_command(
   OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/block_log_dictionary_data.inc"
   COMMAND ${PYTHON_EXECUTABLE} "${CMAKE_SOURCE_DIR}/scripts/blocklog/make_dictionary_inc.py"
           "${CMAKE_CURRENT_BINARY_DIR}/block_log_dictionary_data.inc" "${BLOCK_LOG_DICT_DIR}/dictionaries.txt"
   DEPENDS ${BLOCK_LOG_DICT_FILES} "${BLOCK_LOG_DICT_DIR}/dictionaries.txt" "${CMAKE_SOURCE_DIR}/scripts/blocklog/make_dictionary_inc.py"
   COMMENT "Generating the block log dictionary table" )
set_source_files_properties( block_log_dictionaries.cpp PROPERTIES
   OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/block_log_dictionary_data.inc" )

target_link_libraries( steem_chain steem_jsonball steem_protocol fc chainbase steem_schema appbase mira zstd_static
                       ${PATCH_MERGE_LIB} )
target_include_directories( steem_chain
                            PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}"
                            PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_BINARY_DIR}/include" )

if( CLANG_TIDY_EXE )
   set_target_properties(
      steem_chain PROPERTIES
      CXX_CLANG_TIDY "${DO_CLANG_TIDY}"
   )
endif( CLANG_TIDY_EXE )

if(MSVC)
  set_source_files_properties( database.cpp PROPERTIES COMPILE_FLAGS "/bigobj" )
endif(MSVC)

INSTALL( TARGETS
   steem_chain

   RUNTIME DESTINATION bin
   LIBRARY DESTINATION lib
   ARCHIVE DESTINATION lib
)
INSTALL( FILES ${HEADERS} DESTINATION "include/steem/chain" )
