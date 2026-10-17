#pragma once

#include <cstddef>
#include <cstdint>

namespace steem { namespace chain { namespace block_log_codec {

/// A zstd dictionary built into the binary. The ids and their data must never change once blocks
/// have been written with them: a dictionary is only ever added (with a new id), never replaced.
struct dictionary_data
{
   uint8_t               id;
   uint32_t              first_block;   // used for blocks from this height up to the next dictionary's first_block
   const unsigned char*  data;
   size_t                size;
};

size_t                  dictionaries_available();
const dictionary_data*  find_dictionary( uint8_t id );
const dictionary_data*  best_dictionary_for_block( uint32_t block_num );

} } }
