#include <steem/chain/block_log_dictionaries.hpp>

// The dictionary table. It is filled in by the dictionary training tool (scripts/blocklog/train_dictionaries.py)
// which writes block_log_dictionary_data.inc. With no dictionaries the table is empty and blocks are
// compressed without one.
namespace steem { namespace chain { namespace block_log_codec {

namespace {
#include "block_log_dictionary_data.inc"
}

size_t dictionaries_available()
{
   return sizeof( dictionary_table ) / sizeof( dictionary_table[0] ) - 1; // last entry is a terminator
}

const dictionary_data* find_dictionary( uint8_t id )
{
   for( size_t i = 0; i < dictionaries_available(); ++i )
      if( dictionary_table[i].id == id ) return &dictionary_table[i];
   return nullptr;
}

const dictionary_data* best_dictionary_for_block( uint32_t block_num )
{
   const dictionary_data* best = nullptr;
   for( size_t i = 0; i < dictionaries_available(); ++i )
      if( dictionary_table[i].first_block <= block_num ) best = &dictionary_table[i];
   return best;
}

} } }
