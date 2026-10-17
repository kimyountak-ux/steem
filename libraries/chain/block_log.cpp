#include <steem/chain/block_log.hpp>
#include <fstream>
#include <fc/io/raw.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <boost/thread/mutex.hpp>
#include <boost/interprocess/sync/scoped_lock.hpp>
#include <boost/interprocess/sync/lock_options.hpp>

#define LOG_READ  (std::ios::in | std::ios::binary)
#define LOG_WRITE (std::ios::out | std::ios::binary | std::ios::app)

namespace steem { namespace chain {

   typedef boost::interprocess::scoped_lock< boost::mutex > scoped_lock;

   boost::interprocess::defer_lock_type defer_lock;

   namespace {
      std::atomic< int > write_level{ 0 };
   }

   void block_log::set_write_compression_level( int level )
   {
      write_level = level < 0 ? 0 : ( level > 22 ? 22 : level );
   }

   int block_log::write_compression_level()
   {
      return write_level;
   }

   namespace detail {
      class block_log_impl {
         public:
            optional< signed_block > head;
            block_id_type            head_id;
            std::fstream             block_stream;
            std::fstream             index_stream;
            fc::path                 block_file;
            fc::path                 index_file;
            bool                     block_write = false;
            bool                     index_write = false;

            bool                     use_locking = true;

            boost::mutex             mtx;

            inline void check_block_read()
            {
               try
               {
                  if( block_write )
                  {
                     block_stream.close();
                     block_stream.open( block_file.generic_string().c_str(), LOG_READ );
                     block_write = false;
                  }
               }
               FC_LOG_AND_RETHROW()
            }

            inline void check_block_write()
            {
               try
               {
                  if( !block_write )
                  {
                     block_stream.close();
                     block_stream.open( block_file.generic_string().c_str(), LOG_WRITE );
                     block_write = true;
                  }
               }
               FC_LOG_AND_RETHROW()
            }

            inline void check_index_read()
            {
               try
               {
                  if( index_write )
                  {
                     index_stream.close();
                     index_stream.open( index_file.generic_string().c_str(), LOG_READ );
                     index_write = false;
                  }
               }
               FC_LOG_AND_RETHROW()
            }

            inline void check_index_write()
            {
               try
               {
                  if( !index_write )
                  {
                     index_stream.close();
                     index_stream.open( index_file.generic_string().c_str(), LOG_WRITE );
                     index_write = true;
                  }
               }
               FC_LOG_AND_RETHROW()
            }
      };
   }

   block_log::block_log()
   :my( new detail::block_log_impl() )
   {
      my->block_stream.exceptions( std::fstream::failbit | std::fstream::badbit );
      my->index_stream.exceptions( std::fstream::failbit | std::fstream::badbit );
   }

   block_log::~block_log()
   {
      flush();
   }

   void block_log::open( const fc::path& file )
   {
      if( my->block_stream.is_open() )
         my->block_stream.close();
      if( my->index_stream.is_open() )
         my->index_stream.close();

      my->block_file = file;
      my->index_file = fc::path( file.generic_string() + ".index" );

      my->block_stream.open( my->block_file.generic_string().c_str(), LOG_WRITE );
      my->index_stream.open( my->index_file.generic_string().c_str(), LOG_WRITE );
      my->block_write = true;
      my->index_write = true;

      /* On startup of the block log, there are several states the log file and the index file can be
       * in relation to eachother.
       *
       *                          Block Log
       *                     Exists       Is New
       *                 +------------+------------+
       *          Exists |    Check   |   Delete   |
       *   Index         |    Head    |    Index   |
       *    File         +------------+------------+
       *          Is New |   Replay   |     Do     |
       *                 |    Log     |   Nothing  |
       *                 +------------+------------+
       *
       * Checking the heads of the files has several conditions as well.
       *  - If they are the same, do nothing.
       *  - If the index file head is not in the log file, delete the index and replay.
       *  - If the index file head is in the log, but not up to date, replay from index head.
       */
      auto log_size = fc::file_size( my->block_file );
      auto index_size = fc::file_size( my->index_file );

      if( log_size )
      {
         ilog( "Log is nonempty" );
         my->head = read_head();
         my->head_id = my->head->id();

         if( index_size )
         {
            my->check_block_read();
            my->check_index_read();

            ilog( "Index is nonempty" );
            uint64_t block_pos;
            my->block_stream.seekg( -sizeof( uint64_t), std::ios::end );
            my->block_stream.read( (char*)&block_pos, sizeof( block_pos ) );

            uint64_t index_pos;
            my->index_stream.seekg( -sizeof( uint64_t), std::ios::end );
            my->index_stream.read( (char*)&index_pos, sizeof( index_pos ) );

            if( block_pos < index_pos )
            {
               ilog( "block_pos < index_pos, close and reopen index_stream" );
               construct_index();
            }
            else if( block_pos > index_pos )
            {
               ilog( "Index is incomplete" );
               construct_index();
            }
         }
         else
         {
            ilog( "Index is empty" );
            construct_index();
         }
      }
      else if( index_size )
      {
         ilog( "Index is nonempty, remove and recreate it" );
         my->index_stream.close();
         fc::remove_all( my->index_file );
         my->index_stream.open( my->index_file.generic_string().c_str(), LOG_WRITE );
         my->index_write = true;
      }
   }

   void block_log::close()
   {
      my.reset( new detail::block_log_impl() );
   }

   bool block_log::is_open()const
   {
      return my->block_stream.is_open();
   }

   uint64_t block_log::append( const signed_block& b )
   {
      try
      {
         scoped_lock lock( my->mtx, defer_lock );

         if( my->use_locking )
         {
            lock.lock();;
         }

         auto raw = fc::raw::pack_to_vector( b );
         block_attributes attrs;
         std::vector< char > packed;
         const int level = write_compression_level();
         if( level > 0 )
         {
            const auto dictionary = block_log_codec::dictionary_for_block( b.block_num() );
            if( block_log_codec::compress( raw.data(), raw.size(), level, dictionary, packed ) )
            {
               attrs.compressed = true;
               attrs.dictionary = dictionary;
            }
         }
         signed_block head_copy( b );
         return append_payload( b.block_num(), attrs.compressed ? packed : raw, attrs, &head_copy );
      }
      FC_LOG_AND_RETHROW()
   }

   uint64_t block_log::append_encoded( uint32_t block_num, const std::vector< char >& payload,
                                       const block_attributes& attrs, const std::vector< char >* raw )
   {
      try
      {
         scoped_lock lock( my->mtx, defer_lock );

         if( my->use_locking )
         {
            lock.lock();;
         }

         if( raw == nullptr )
            return append_payload( block_num, payload, attrs, nullptr );

         signed_block b;
         fc::raw::unpack_from_vector( *raw, b );
         FC_ASSERT( b.block_num() == block_num, "Wrong block appended to block log.",
                    ( "block", b.block_num() )( "expected", block_num ) );
         return append_payload( block_num, payload, attrs, &b );
      }
      FC_LOG_AND_RETHROW()
   }

   // Writes one entry: the block bytes, then the 8 byte value that is also written to the index.
   // The caller holds the lock.
   uint64_t block_log::append_payload( uint32_t block_num, const std::vector< char >& payload,
                                       const block_attributes& attrs, signed_block* head_block )
   {
      my->check_block_write();
      my->check_index_write();

      uint64_t pos = my->block_stream.tellp();
      FC_ASSERT( static_cast<uint64_t>(my->index_stream.tellp()) == sizeof( uint64_t ) * ( block_num - 1 ),
         "Append to index file occuring at wrong position.",
         ( "position", (uint64_t) my->index_stream.tellp() )( "expected",( block_num - 1 ) * sizeof( uint64_t ) ) );
      FC_ASSERT( pos <= block_log_codec::POSITION_MASK, "Block log is too large for its position field" );
      const uint64_t tagged = block_log_codec::make_tagged_pos( pos, attrs );
      my->block_stream.write( payload.data(), payload.size() );
      my->block_stream.write( (char*)&tagged, sizeof( tagged ) );
      my->index_stream.write( (char*)&tagged, sizeof( tagged ) );
      if( head_block != nullptr )
      {
         my->head = std::move( *head_block );
         my->head_id = my->head->id();
      }

      return pos;
   }

   void block_log::flush()
   {
      scoped_lock lock( my->mtx, defer_lock );

            if( my->use_locking )
            {
               lock.lock();;
            }

      my->block_stream.flush();
      my->index_stream.flush();
   }

   std::pair< signed_block, uint64_t > block_log::read_block( uint64_t pos )const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      return read_block_helper( pos );
   }

   std::pair< signed_block, uint64_t > block_log::read_block_helper( uint64_t pos )const
   {
      try
      {
         my->check_block_read();

         my->block_stream.seekg( pos );
         std::pair<signed_block,uint64_t> result;
         fc::raw::unpack( my->block_stream, result.first );
         result.second = uint64_t(my->block_stream.tellg()) + 8;
         return result;
      }
      FC_LOG_AND_RETHROW()
   }

   optional< signed_block > block_log::read_block_by_num( uint32_t block_num )const
   {
      try
      {
         scoped_lock lock( my->mtx, defer_lock );

         if( my->use_locking )
         {
            lock.lock();;
         }

         optional< signed_block > b;
         if( get_tagged_pos_helper( block_num ) != npos )
         {
            std::vector< char > raw;
            read_raw_helper( block_num, raw );
            b = signed_block();
            fc::raw::unpack_from_vector( raw, *b );
            FC_ASSERT( b->block_num() == block_num , "Wrong block was read from block log.", ( "returned", b->block_num() )( "expected", block_num ));
         }
         return b;
      }
      FC_LOG_AND_RETHROW()
   }

   // The 8 byte value of a block as written to the index (position plus attributes), npos if there is no such block.
   uint64_t block_log::get_tagged_pos_helper( uint32_t block_num ) const
   {
      try
      {
         my->check_index_read();

         if( !( my->head.valid() && block_num <= protocol::block_header::num_from_id( my->head_id ) && block_num > 0 ) )
            return npos;
         my->index_stream.seekg( sizeof( uint64_t ) * ( block_num - 1 ) );
         uint64_t pos;
         my->index_stream.read( (char*)&pos, sizeof( pos ) );
         return pos;
      }
      FC_LOG_AND_RETHROW()
   }

   // The bytes of a block as stored. The caller holds the lock.
   void block_log::read_encoded_helper( uint32_t block_num, std::vector< char >& payload, block_attributes& attrs )const
   {
      try
      {
         const uint64_t tagged = get_tagged_pos_helper( block_num );
         FC_ASSERT( tagged != npos, "Block ${n} is not in the block log", ("n", block_num) );
         attrs = block_log_codec::tagged_pos_attributes( tagged );
         const uint64_t start = block_log_codec::tagged_pos_position( tagged );

         // a block ends where the 8 byte value in front of the next block (or the end of the file) begins
         uint64_t end;
         if( block_num < protocol::block_header::num_from_id( my->head_id ) )
         {
            my->check_index_read();
            my->index_stream.seekg( sizeof( uint64_t ) * block_num );
            uint64_t next;
            my->index_stream.read( (char*)&next, sizeof( next ) );
            end = block_log_codec::tagged_pos_position( next ) - sizeof( uint64_t );
         }
         else
         {
            my->check_block_read();
            my->block_stream.seekg( 0, std::ios::end );
            end = uint64_t( my->block_stream.tellg() ) - sizeof( uint64_t );
         }
         FC_ASSERT( end > start, "Damaged block log entry for block ${n}", ("n", block_num) );

         my->check_block_read();
         payload.resize( end - start );
         my->block_stream.seekg( start );
         my->block_stream.read( payload.data(), payload.size() );
      }
      FC_LOG_AND_RETHROW()
   }

   void block_log::read_raw_helper( uint32_t block_num, std::vector< char >& raw )const
   {
      block_attributes attrs;
      std::vector< char > payload;
      read_encoded_helper( block_num, payload, attrs );
      if( attrs.compressed )
         block_log_codec::decompress( payload.data(), payload.size(), attrs.dictionary, raw );
      else
         raw.swap( payload );
   }

   void block_log::read_block_encoded( uint32_t block_num, std::vector< char >& payload, block_attributes& attrs )const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      read_encoded_helper( block_num, payload, attrs );
   }

   std::vector< char > block_log::read_block_raw( uint32_t block_num )const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      std::vector< char > raw;
      read_raw_helper( block_num, raw );
      return raw;
   }

   uint32_t block_log::head_block_num()const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      return my->head.valid() ? protocol::block_header::num_from_id( my->head_id ) : 0;
   }

   uint64_t block_log::get_block_pos( uint32_t block_num ) const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      return get_block_pos_helper( block_num );
   }

   // Position of the start of a block in the file (without the attributes), npos if there is no such block.
   uint64_t block_log::get_block_pos_helper( uint32_t block_num ) const
   {
      const uint64_t tagged = get_tagged_pos_helper( block_num );
      return tagged == npos ? npos : block_log_codec::tagged_pos_position( tagged );
   }

   signed_block block_log::read_head()const
   {
      try
      {
         scoped_lock lock( my->mtx, defer_lock );

         if( my->use_locking )
         {
            lock.lock();;
         }

         my->check_block_read();

         // the last 8 bytes of the file say where (and how) the last block is stored
         my->block_stream.seekg( 0, std::ios::end );
         const uint64_t file_size = my->block_stream.tellg();
         FC_ASSERT( file_size > sizeof( uint64_t ), "Block log is too small to hold a block" );
         uint64_t tagged;
         my->block_stream.seekg( file_size - sizeof( tagged ) );
         my->block_stream.read( (char*)&tagged, sizeof( tagged ) );

         const block_attributes attrs = block_log_codec::tagged_pos_attributes( tagged );
         const uint64_t start = block_log_codec::tagged_pos_position( tagged );
         FC_ASSERT( start < file_size - sizeof( tagged ), "Damaged end of the block log" );
         std::vector< char > payload( file_size - sizeof( tagged ) - start );
         my->block_stream.seekg( start );
         my->block_stream.read( payload.data(), payload.size() );

         signed_block head;
         if( attrs.compressed )
         {
            std::vector< char > raw;
            block_log_codec::decompress( payload.data(), payload.size(), attrs.dictionary, raw );
            fc::raw::unpack_from_vector( raw, head );
         }
         else
            fc::raw::unpack_from_vector( payload, head );
         return head;
      }
      FC_LOG_AND_RETHROW()
   }

   const optional< signed_block >& block_log::head()const
   {
      scoped_lock lock( my->mtx, defer_lock );

      if( my->use_locking )
      {
         lock.lock();;
      }

      return my->head;
   }

   // Rebuilds the index from the main file. Every block is followed by its 8 byte value, and the value
   // of the block before it sits right in front of where this block starts - so the file can be
   // walked from the end to the beginning without reading any block. (Going forward is not possible
   // when blocks are compressed: their length is not part of their bytes.) The file is read in large
   // pieces from the back, which is the sequential way to go.
   void block_log::construct_index()
   {
      try
      {
         ilog( "Reconstructing Block Log Index..." );
         my->index_stream.close();
         fc::remove_all( my->index_file );
         my->index_stream.open( my->index_file.generic_string().c_str(), LOG_WRITE );
         my->index_write = true;

         my->check_block_read();
         my->block_stream.seekg( 0, std::ios::end );
         const uint64_t file_size = my->block_stream.tellg();
         FC_ASSERT( file_size > sizeof( uint64_t ), "Block log is too small to hold a block" );

         const uint64_t WINDOW = 16 * 1024 * 1024;
         std::vector< char > window;
         uint64_t window_start = file_size; // nothing loaded yet
         auto value_at = [&]( uint64_t offset ) -> uint64_t
         {
            if( offset < window_start || offset + sizeof( uint64_t ) > window_start + window.size() )
            {
               const uint64_t end = std::min( file_size, offset + sizeof( uint64_t ) + WINDOW / 2 );
               const uint64_t from = end > WINDOW ? end - WINDOW : 0;
               window.resize( end - from );
               my->block_stream.seekg( from );
               my->block_stream.read( window.data(), window.size() );
               window_start = from;
            }
            uint64_t v;
            memcpy( &v, window.data() + ( offset - window_start ), sizeof( v ) );
            return v;
         };

         std::vector< uint64_t > values; // newest block first
         uint64_t offset = file_size - sizeof( uint64_t );
         while( true )
         {
            const uint64_t tagged = value_at( offset );
            values.push_back( tagged );
            const uint64_t start = block_log_codec::tagged_pos_position( tagged );
            if( start == 0 ) break;
            FC_ASSERT( start >= sizeof( uint64_t ) && start < offset, "Damaged block log: entry at ${s} is out of order", ("s", start) );
            offset = start - sizeof( uint64_t );
         }

         std::reverse( values.begin(), values.end() );
         const size_t CHUNK = 1 << 20;
         for( size_t i = 0; i < values.size(); i += CHUNK )
            my->index_stream.write( (const char*)( values.data() + i ),
                                    sizeof( uint64_t ) * std::min( CHUNK, values.size() - i ) );
         my->index_stream.flush();
         ilog( "Block log index has ${n} blocks", ("n", values.size()) );
      }
      FC_LOG_AND_RETHROW()
   }

   void block_log::set_locking( bool use_locking )
   {
      my->use_locking = true;
   }
} } // steem::chain
