#pragma once

#include <fc/optional.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace steem { namespace chain {

/**
 * Per-block compression of the block log.
 *
 * Every entry of the block log is   [ block bytes ][ 8 byte trailer ]   and the index file holds the
 * same 8 byte value for every block. In a log without compression that value is simply the position
 * where the block starts. With compression the unused top bits of the same value carry the attributes
 * of the block, so a log can freely mix compressed and plain blocks and no separate size field is
 * needed (the size of a block is the distance to the next entry):
 *
 *     bit 63        1 = the block bytes are zstd compressed, 0 = plain
 *     bit 56        1 = a dictionary was used, its number is in bits 48..55
 *     bits 48..55   dictionary number
 *     bits 0..47    position of the start of the block in the file
 *
 * A compressed block is a zstd frame without magic number, content size, dictionary id or checksum
 * (they only cost bytes - the attributes above already say everything the reader needs).
 */
struct block_attributes
{
   bool                  compressed = false;
   fc::optional<uint8_t> dictionary;
};

namespace block_log_codec {

constexpr uint64_t POSITION_MASK   = 0x0000ffffffffffffULL;
constexpr uint64_t COMPRESSED_FLAG = 0x8000000000000000ULL;
constexpr uint64_t DICTIONARY_FLAG = 0x0100000000000000ULL;

inline uint64_t make_tagged_pos( uint64_t pos, const block_attributes& attrs )
{
   uint64_t v = pos & POSITION_MASK;
   if( attrs.compressed ) v |= COMPRESSED_FLAG;
   if( attrs.dictionary.valid() ) v |= DICTIONARY_FLAG | ( uint64_t( *attrs.dictionary ) << 48 );
   return v;
}

inline uint64_t tagged_pos_position( uint64_t tagged ) { return tagged & POSITION_MASK; }

inline block_attributes tagged_pos_attributes( uint64_t tagged )
{
   block_attributes a;
   a.compressed = ( tagged & COMPRESSED_FLAG ) != 0;
   if( tagged & DICTIONARY_FLAG ) a.dictionary = uint8_t( ( tagged >> 48 ) & 0xff );
   return a;
}

/// Dictionaries built into this binary (trained on real blocks, see block_log_dictionaries.cpp).
size_t dictionary_count();
/// The dictionary best suited for a block of this height, none if no dictionaries are built in.
fc::optional<uint8_t> dictionary_for_block( uint32_t block_num );

/**
 * Compress the serialized block. Returns false (and leaves `out` unspecified) when the result would
 * not be smaller than the input - the caller then stores the block plain.
 * level: 1..22 (ignored when a dictionary is used - dictionaries are prepared at a fixed level).
 */
bool compress( const char* raw, size_t size, int level, const fc::optional<uint8_t>& dictionary, std::vector<char>& out );

/// Throws fc::exception if the data is not a valid compressed block.
void decompress( const char* data, size_t size, const fc::optional<uint8_t>& dictionary, std::vector<char>& out );

} } }
