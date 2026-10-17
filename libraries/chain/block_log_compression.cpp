#include <steem/chain/block_log_compression.hpp>
#include <steem/chain/block_log_dictionaries.hpp>

#include <fc/exception/exception.hpp>

#include <map>
#include <memory>
#include <mutex>

#ifndef ZSTD_STATIC_LINKING_ONLY
#  define ZSTD_STATIC_LINKING_ONLY
#endif
#include <zstd.h>

namespace steem { namespace chain { namespace block_log_codec {

namespace {

   struct cctx_holder
   {
      ZSTD_CCtx* ctx = ZSTD_createCCtx();
      ~cctx_holder() { ZSTD_freeCCtx( ctx ); }
   };

   struct dctx_holder
   {
      ZSTD_DCtx* ctx = ZSTD_createDCtx();
      ~dctx_holder() { ZSTD_freeDCtx( ctx ); }
   };

   // one dictionary object per (dictionary, level), shared by all threads (they are read only)
   std::mutex                                           dict_mutex;
   std::map< std::pair< int, int >, ZSTD_CDict* >       cdicts;
   std::map< int, ZSTD_DDict* >                         ddicts;

   const dictionary_data& dictionary_or_throw( uint8_t id )
   {
      const dictionary_data* d = find_dictionary( id );
      FC_ASSERT( d != nullptr, "Block log uses compression dictionary ${id} which this build does not have", ("id", id) );
      return *d;
   }

   ZSTD_CDict* get_cdict( uint8_t id, int level )
   {
      std::lock_guard< std::mutex > lock( dict_mutex );
      auto key = std::make_pair( int( id ), level );
      auto it = cdicts.find( key );
      if( it != cdicts.end() ) return it->second;
      const dictionary_data& d = dictionary_or_throw( id );
      ZSTD_CDict* c = ZSTD_createCDict( d.data, d.size, level );
      FC_ASSERT( c != nullptr, "Could not prepare compression dictionary ${id}", ("id", id) );
      cdicts[ key ] = c;
      return c;
   }

   ZSTD_DDict* get_ddict( uint8_t id )
   {
      std::lock_guard< std::mutex > lock( dict_mutex );
      auto it = ddicts.find( id );
      if( it != ddicts.end() ) return it->second;
      const dictionary_data& d = dictionary_or_throw( id );
      ZSTD_DDict* c = ZSTD_createDDict( d.data, d.size );
      FC_ASSERT( c != nullptr, "Could not prepare decompression dictionary ${id}", ("id", id) );
      ddicts[ id ] = c;
      return c;
   }

} // anonymous

size_t dictionary_count()
{
   return dictionaries_available();
}

fc::optional<uint8_t> dictionary_for_block( uint32_t block_num )
{
   const dictionary_data* d = best_dictionary_for_block( block_num );
   if( d == nullptr ) return fc::optional<uint8_t>();
   return d->id;
}

bool compress( const char* raw, size_t size, int level, const fc::optional<uint8_t>& dictionary, std::vector<char>& out )
{
   thread_local cctx_holder holder;
   ZSTD_CCtx* ctx = holder.ctx;
   FC_ASSERT( ctx != nullptr, "zstd compression context" );

   ZSTD_CCtx_reset( ctx, ZSTD_reset_session_and_parameters );
   ZSTD_CCtx_setParameter( ctx, ZSTD_c_format, ZSTD_f_zstd1_magicless );
   ZSTD_CCtx_setParameter( ctx, ZSTD_c_contentSizeFlag, 0 );
   ZSTD_CCtx_setParameter( ctx, ZSTD_c_checksumFlag, 0 );
   ZSTD_CCtx_setParameter( ctx, ZSTD_c_dictIDFlag, 0 );
   if( dictionary.valid() )
      FC_ASSERT( !ZSTD_isError( ZSTD_CCtx_refCDict( ctx, get_cdict( *dictionary, level ) ) ), "zstd dictionary" );
   else
      ZSTD_CCtx_setParameter( ctx, ZSTD_c_compressionLevel, level );

   out.resize( ZSTD_compressBound( size ) );
   const size_t n = ZSTD_compress2( ctx, out.data(), out.size(), raw, size );
   if( ZSTD_isError( n ) ) return false;
   if( n >= size ) return false;
   out.resize( n );
   return true;
}

void decompress( const char* data, size_t size, const fc::optional<uint8_t>& dictionary, std::vector<char>& out )
{
   thread_local dctx_holder holder;
   ZSTD_DCtx* ctx = holder.ctx;
   FC_ASSERT( ctx != nullptr, "zstd decompression context" );

   ZSTD_DCtx_reset( ctx, ZSTD_reset_session_and_parameters );
   ZSTD_DCtx_setParameter( ctx, ZSTD_d_format, ZSTD_f_zstd1_magicless );
   if( dictionary.valid() )
      FC_ASSERT( !ZSTD_isError( ZSTD_DCtx_refDDict( ctx, get_ddict( *dictionary ) ) ), "zstd dictionary" );

   // the size of the block is not stored: decompress in steps until the frame is complete
   out.clear();
   out.resize( std::max< size_t >( 16 * 1024, size * 4 ) );
   ZSTD_inBuffer  in  = { data, size, 0 };
   size_t         produced = 0;
   size_t         ret;
   do
   {
      if( produced == out.size() ) out.resize( out.size() * 2 );
      ZSTD_outBuffer o = { out.data(), out.size(), produced };
      ret = ZSTD_decompressStream( ctx, &o, &in );
      FC_ASSERT( !ZSTD_isError( ret ), "Corrupt compressed block in the block log: ${e}", ("e", ZSTD_getErrorName( ret )) );
      produced = o.pos;
   }
   while( ret != 0 );
   FC_ASSERT( in.pos == in.size, "Compressed block in the block log has trailing bytes" );
   out.resize( produced );
}

} } }
