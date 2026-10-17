#pragma once
#include <fc/filesystem.hpp>
#include <steem/protocol/block.hpp>
#include <steem/chain/block_log_compression.hpp>

namespace steem { namespace chain {

   using namespace steem::protocol;

   namespace detail { class block_log_impl; }

   /* The block log is an external append only log of the blocks. Blocks should only be written
    * to the log after they irreverisble as the log is append only. The log is a doubly linked
    * list of blocks. There is a secondary index file of only block positions that enables O(1)
    * random access lookup by block number.
    *
    * +---------+----------------+---------+----------------+-----+------------+-------------------+
    * | Block 1 | Pos of Block 1 | Block 2 | Pos of Block 2 | ... | Head Block | Pos of Head Block |
    * +---------+----------------+---------+----------------+-----+------------+-------------------+
    *
    * +----------------+----------------+-----+-------------------+
    * | Pos of Block 1 | Pos of Block 2 | ... | Pos of Head Block |
    * +----------------+----------------+-----+-------------------+
    *
    * The block log can be walked in order by deserializing a block, skipping 8 bytes, deserializing a
    * block, repeat... The head block of the file can be found by seeking to the position contained
    * in the last 8 bytes the file. The block log can be read backwards by jumping back 8 bytes, following
    * the position, reading the block, jumping back 8 bytes, etc.
    *
    * Blocks can be accessed at random via block number through the index file. Seek to 8 * (block_num - 1)
    * to find the position of the block in the main file.
    *
    * The main file is the only file that needs to persist. The index file can be reconstructed during a
    * scan of the main file.
    *
    * Compression: blocks can be stored zstd compressed (see block_log_compression.hpp). The 8 byte
    * values in the file and in the index then also carry the attributes of the block in their unused
    * top bits, so a log may mix plain and compressed blocks. Without compression every value is just
    * the position of the block, exactly as before. Because the length of a compressed block is not
    * known from its bytes, blocks are read by block number through the index (read_block_by_num) -
    * reading "the next block from the end of this one" (read_block) works on plain logs only.
    */

   class block_log {
      public:
         block_log();
         ~block_log();

         void open( const fc::path& file );
         void close();
         bool is_open()const;

         uint64_t append( const signed_block& b );
         void flush();
         /// Plain (not compressed) logs only: the block at file_pos and the position of the next one.
         std::pair< signed_block, uint64_t > read_block( uint64_t file_pos )const;
         optional< signed_block > read_block_by_num( uint32_t block_num )const;

         /**
          * Return offset of block in file, or block_log::npos if it does not exist.
          */
         uint64_t get_block_pos( uint32_t block_num ) const;
         signed_block read_head()const;
         const optional< signed_block >& head()const;

         /**
          * Blocks appended from now on are stored zstd compressed at this level (1..22), 0 = not compressed
          * (the default). Reading never depends on this: compressed and plain blocks are always readable.
          */
         static void set_write_compression_level( int level );
         static int  write_compression_level();

         /// Number of the last block in the log, 0 if the log is empty.
         uint32_t head_block_num()const;

         /// The serialized block exactly as the chain produced it (decompressed if it is stored compressed).
         std::vector< char > read_block_raw( uint32_t block_num )const;

         /// The block as it is stored: its bytes and whether/how they are compressed.
         void read_block_encoded( uint32_t block_num, std::vector< char >& payload, block_attributes& attrs )const;

         /**
          * Append an already encoded block (used by the conversion tool). `raw` is the same block
          * uncompressed, it is only used to remember the new head - pass nullptr to skip that when
          * appending many blocks in a row (the head is then only known after the log is opened again).
          */
         uint64_t append_encoded( uint32_t block_num, const std::vector< char >& payload,
                                  const block_attributes& attrs, const std::vector< char >* raw );

         /*
          * Used by the database to skip locking when reindexing
          * APIs don't work at this point, so there is no danger.
          */
         void set_locking( bool );

         static const uint64_t npos = std::numeric_limits<uint64_t>::max();

      private:
         void construct_index();

         std::pair< signed_block, uint64_t > read_block_helper( uint64_t file_pos )const;
         uint64_t get_block_pos_helper( uint32_t block_num ) const;
         uint64_t get_tagged_pos_helper( uint32_t block_num ) const;
         void read_encoded_helper( uint32_t block_num, std::vector< char >& payload, block_attributes& attrs )const;
         void read_raw_helper( uint32_t block_num, std::vector< char >& raw )const;
         uint64_t append_payload( uint32_t block_num, const std::vector< char >& payload,
                                  const block_attributes& attrs, signed_block* head_block );

         std::unique_ptr<detail::block_log_impl> my;
   };

} }
