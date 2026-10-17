// Converts a block log to (or between) compressed and plain blocks, verifies a block log and reports on it.
//
//   compress_block_log convert --input SRC/block_log --output DST/block_log --level 3 [--threads 8]
//   compress_block_log verify  --input DST/block_log [--reference SRC/block_log]   (input: a log made by this tool)
//   compress_block_log stats   --input DST/block_log
//   compress_block_log train   --input SRC/block_log --from A --to B --out FILE.dict [--samples-mb 40] [--dict-size 220000]
//   compress_block_log estimate --input SRC/block_log --from A --to B [--step 50] [--level 15] [--no-dictionary]
//
// The SOURCE is only ever read, with plain pread() calls on the block log and its index - it is never
// opened by the block_log class, which could rewrite the index of a log that a running steemd is still
// appending to. Convert only handles the blocks the index already lists (and not the very last one, whose
// end is only known once the next one is written), so it is safe on a live node; the remaining blocks
// are added with a second run (--from) when the node is stopped for the switch.

#include <steem/chain/block_log.hpp>

#include <fc/exception/exception.hpp>
#include <fc/io/raw.hpp>

#include <boost/program_options.hpp>

#ifndef ZDICT_STATIC_LINKING_ONLY
#  define ZDICT_STATIC_LINKING_ONLY
#endif
#include <zdict.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <random>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

namespace bpo = boost::program_options;
using namespace steem::chain;
using steem::protocol::signed_block;

namespace {

struct source_log
{
   int      block_fd = -1;
   int      index_fd = -1;
   uint64_t blocks   = 0;   // entries in the index
   uint64_t size     = 0;   // bytes in the block file

   explicit source_log( const std::string& path )
   {
      block_fd = ::open( path.c_str(), O_RDONLY );
      index_fd = ::open( ( path + ".index" ).c_str(), O_RDONLY );
      FC_ASSERT( block_fd >= 0, "cannot open ${p}", ("p", path) );
      FC_ASSERT( index_fd >= 0, "cannot open ${p}.index", ("p", path) );
      struct stat st;
      FC_ASSERT( ::fstat( block_fd, &st ) == 0, "stat" );
      size = st.st_size;
      FC_ASSERT( ::fstat( index_fd, &st ) == 0, "stat" );
      blocks = st.st_size / sizeof( uint64_t );
   }
   ~source_log() { if( block_fd >= 0 ) ::close( block_fd ); if( index_fd >= 0 ) ::close( index_fd ); }

   void pread_all( int fd, void* buf, size_t n, uint64_t off ) const
   {
      char* p = static_cast< char* >( buf );
      while( n > 0 )
      {
         ssize_t r = ::pread( fd, p, n, off );
         FC_ASSERT( r > 0, "read failed at ${o}", ("o", off) );
         p += r; n -= r; off += r;
      }
   }

   // index values of blocks first..last, plus one more telling where `last` ends: the next block's value or, for the
   // very last block of the log, the end of the file (its trailer then is the file's last 8 bytes). The end of
   // the file is only a safe end of block while no steemd appends to the log.
   std::vector< uint64_t > values( uint64_t first, uint64_t last ) const
   {
      FC_ASSERT( first >= 1 && last <= blocks, "block ${l} is not in the index", ("l", last) );
      const bool at_end = last == blocks;
      std::vector< uint64_t > v( last - first + 2 );
      pread_all( index_fd, v.data(), ( last - first + 1 + ( at_end ? 0 : 1 ) ) * sizeof( uint64_t ), ( first - 1 ) * sizeof( uint64_t ) );
      if( at_end ) v.back() = size;
      return v;
   }
};

struct encoded_block
{
   std::vector< char >  payload;   // as stored in the source
   block_attributes     attrs;
   std::vector< char >  raw;       // uncompressed
};

void decode( encoded_block& b )
{
   if( b.attrs.compressed )
      block_log_codec::decompress( b.payload.data(), b.payload.size(), b.attrs.dictionary, b.raw );
   else
      b.raw = b.payload;
}

// Block n of a source log as the chain serialized it (decompressed if needed), read only.
std::vector< char > source_raw( const source_log& src, uint64_t n )
{
   auto v = src.values( n, n );
   encoded_block b;
   const uint64_t start = block_log_codec::tagged_pos_position( v[0] );
   const uint64_t end = block_log_codec::tagged_pos_position( v[1] ) - sizeof( uint64_t );
   FC_ASSERT( end > start && end <= src.size, "Damaged source entry for block ${n}", ("n", n) );
   b.attrs = block_log_codec::tagged_pos_attributes( v[0] );
   b.payload.resize( end - start );
   src.pread_all( src.block_fd, b.payload.data(), end - start, start );
   decode( b );
   return b.raw;
}

void report( const char* what, uint64_t done, uint64_t total, uint64_t in_bytes, uint64_t out_bytes,
             std::chrono::steady_clock::time_point t0 )
{
   const double secs = std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
   std::cerr << what << " " << done << " / " << total << " blocks   "
             << ( in_bytes >> 20 ) << " MB -> " << ( out_bytes >> 20 ) << " MB";
   if( in_bytes ) std::cerr << " (" << ( 100.0 * out_bytes / in_bytes ) << "%)";
   std::cerr << "   " << uint64_t( done / std::max( secs, 0.001 ) ) << " blocks/s\n";
}

int convert( const bpo::variables_map& o )
{
   source_log src( o["input"].as< std::string >() );
   const int level = o["level"].as< int >();
   const bool use_dict = !o.count( "no-dictionary" );
   const unsigned threads = std::max( 1, o["threads"].as< int >() );
   const uint64_t batch = 4096;

   block_log dst;
   dst.open( fc::path( o["output"].as< std::string >() ) );
   const uint64_t resume = dst.head_block_num() + 1;
   const uint64_t first = std::max< uint64_t >( resume, o.count( "from" ) ? o["from"].as< uint64_t >() : 1 );
   uint64_t last = src.blocks - 1; // the last listed block is left for the final run (--include-last)
   if( o.count( "to" ) ) last = std::min< uint64_t >( last, o["to"].as< uint64_t >() );
   if( o.count( "include-last" ) ) last = src.blocks;
   FC_ASSERT( first == resume, "The output ends at block ${h}; --from must be ${n}", ("h", dst.head_block_num())("n", resume) );
   std::cerr << "source has " << src.blocks << " blocks; converting " << first << " .. " << last
             << " at zstd level " << level << ( use_dict ? " with dictionaries" : " without dictionaries" ) << "\n";

   const auto t0 = std::chrono::steady_clock::now();
   uint64_t in_bytes = 0, out_bytes = 0, done = 0, next_report = 200000;
   std::vector< encoded_block > blocks;
   std::vector< std::vector< char > > packed;
   std::vector< char > attrs_compressed;

   for( uint64_t n = first; n <= last; n += batch )
   {
      const uint64_t to = std::min( last, n + batch - 1 );
      auto v = src.values( n, to );
      const uint64_t count = to - n + 1;
      blocks.assign( count, encoded_block() );
      // read
      for( uint64_t i = 0; i < count; ++i )
      {
         const uint64_t start = block_log_codec::tagged_pos_position( v[i] );
         uint64_t end = block_log_codec::tagged_pos_position( v[i + 1] ) - sizeof( uint64_t );
         FC_ASSERT( end > start && end <= src.size, "Damaged source entry for block ${n}", ("n", n + i) );
         blocks[i].attrs = block_log_codec::tagged_pos_attributes( v[i] );
         blocks[i].payload.resize( end - start );
         src.pread_all( src.block_fd, blocks[i].payload.data(), end - start, start );
         in_bytes += end - start;
      }
      // decode + compress in parallel
      packed.assign( count, std::vector< char >() );
      attrs_compressed.assign( count, 0 );
      std::atomic< uint64_t > cursor{ 0 };
      std::vector< std::thread > pool;
      std::atomic< bool > failed{ false };
      for( unsigned t = 0; t < threads; ++t )
         pool.emplace_back( [&]()
         {
            try
            {
               for( uint64_t i; ( i = cursor++ ) < count; )
               {
                  decode( blocks[i] );
                  if( level > 0 )
                  {
                     fc::optional< uint8_t > dict;
                     if( use_dict ) dict = block_log_codec::dictionary_for_block( uint32_t( n + i ) );
                     if( block_log_codec::compress( blocks[i].raw.data(), blocks[i].raw.size(), level, dict, packed[i] ) )
                     {
                        attrs_compressed[i] = 1;
                        blocks[i].attrs.dictionary = dict;
                     }
                  }
               }
            }
            catch( const fc::exception& e ) { std::cerr << e.to_detail_string() << "\n"; failed = true; }
         } );
      for( auto& t : pool ) t.join();
      FC_ASSERT( !failed, "conversion failed" );
      // write in order
      for( uint64_t i = 0; i < count; ++i )
      {
         block_attributes a;
         a.compressed = attrs_compressed[i] != 0;
         if( a.compressed ) a.dictionary = blocks[i].attrs.dictionary;
         const auto& payload = a.compressed ? packed[i] : blocks[i].raw;
         dst.append_encoded( uint32_t( n + i ), payload, a, nullptr );
         out_bytes += payload.size();
      }
      done += count;
      if( done >= next_report ) { report( "converted", done, last - first + 1, in_bytes, out_bytes, t0 ); next_report += 200000; }
   }
   dst.flush();
   report( "converted (done)", done, last - first + 1, in_bytes, out_bytes, t0 );
   return 0;
}

// Checks every block: its number, that it points at the block before it (so the chain is unbroken from
// the first block to the last), and - with --reference - that its serialized bytes are the same as in the
// other log. Ranges run in parallel; the first block of a range is checked against the last of the
// previous one at the end.
int verify( const bpo::variables_map& o )
{
   block_log log;
   log.open( fc::path( o["input"].as< std::string >() ) );
   // the reference (usually the original, possibly the live node's log) is only ever read with pread
   std::unique_ptr< source_log > ref;
   if( o.count( "reference" ) ) ref.reset( new source_log( o["reference"].as< std::string >() ) );
   const uint64_t head = log.head_block_num();
   uint64_t last = o.count( "to" ) ? std::min< uint64_t >( head, o["to"].as< uint64_t >() ) : head;
   if( ref && !o.count( "to" ) ) last = std::min< uint64_t >( last, ref->blocks - 1 );
   const uint64_t first = o.count( "from" ) ? o["from"].as< uint64_t >() : 1;
   std::cerr << "verifying blocks " << first << " .. " << last << ( ref ? " against the reference" : "" ) << "\n";

   const unsigned threads = std::max( 1, o["threads"].as< int >() );
   const uint64_t span = ( last - first + 1 + threads - 1 ) / threads;
   struct range_result { fc::ripemd160 first_prev; fc::ripemd160 last_id; uint64_t errors = 0; uint64_t blocks = 0; };
   std::vector< range_result > results( threads );
   std::vector< std::thread > pool;
   const auto t0 = std::chrono::steady_clock::now();
   std::atomic< uint64_t > progress{ 0 };

   for( unsigned t = 0; t < threads; ++t )
      pool.emplace_back( [&, t]()
      {
         const uint64_t a = first + t * span;
         const uint64_t b = std::min( last, a + span - 1 );
         if( a > b ) return;
         // each thread has its own reader of the log under test
         block_log my_log;
         my_log.open( fc::path( o["input"].as< std::string >() ) );
         fc::ripemd160 prev_id;
         for( uint64_t n = a; n <= b; ++n )
         {
            try
            {
               auto raw = my_log.read_block_raw( uint32_t( n ) );
               signed_block blk;
               fc::raw::unpack_from_vector( raw, blk );
               if( blk.block_num() != n ) throw std::runtime_error( "block number" );
               if( n == a ) results[t].first_prev = blk.previous;
               else if( blk.previous != prev_id ) throw std::runtime_error( "does not follow the block before it" );
               prev_id = blk.id();
               if( ref && source_raw( *ref, n ) != raw ) throw std::runtime_error( "differs from the reference" );
            }
            catch( const std::exception& e )
            {
               if( results[t].errors++ < 10 ) std::cerr << "block " << n << ": " << e.what() << "\n";
            }
            catch( const fc::exception& e )
            {
               if( results[t].errors++ < 10 ) std::cerr << "block " << n << ": " << e.to_detail_string() << "\n";
            }
            results[t].blocks++;
            if( ++progress % 1000000 == 0 ) report( "verified", progress, last - first + 1, 0, 0, t0 );
         }
         results[t].last_id = prev_id;
      } );
   for( auto& t : pool ) t.join();

   uint64_t errors = 0, blocks = 0;
   for( unsigned t = 0; t < threads; ++t )
   {
      errors += results[t].errors;
      blocks += results[t].blocks;
      if( t > 0 && results[t].blocks && results[t - 1].blocks && results[t].first_prev != results[t - 1].last_id )
      {
         std::cerr << "chain is broken between range " << ( t - 1 ) << " and " << t << "\n";
         ++errors;
      }
   }
   std::cerr << "checked " << blocks << " blocks, " << errors << " problems\n";
   return errors == 0 ? 0 : 1;
}

int stats( const bpo::variables_map& o )
{
   source_log src( o["input"].as< std::string >() );
   uint64_t plain = 0, comp = 0, with_dict = 0, plain_bytes = 0, comp_bytes = 0;
   const uint64_t last = src.blocks - 1;
   for( uint64_t n = 1; n <= last; n += 65536 )
   {
      const uint64_t to = std::min( last, n + 65535 );
      auto v = src.values( n, to );
      for( uint64_t i = 0; i <= to - n; ++i )
      {
         const uint64_t size = block_log_codec::tagged_pos_position( v[i + 1] ) - sizeof( uint64_t ) - block_log_codec::tagged_pos_position( v[i] );
         auto a = block_log_codec::tagged_pos_attributes( v[i] );
         if( a.compressed ) { ++comp; comp_bytes += size; if( a.dictionary.valid() ) ++with_dict; }
         else { ++plain; plain_bytes += size; }
      }
   }
   std::cout << "blocks listed in the index: " << src.blocks << "\n"
             << "plain:      " << plain << " blocks, " << ( plain_bytes >> 20 ) << " MB\n"
             << "compressed: " << comp << " blocks, " << ( comp_bytes >> 20 ) << " MB (" << with_dict << " with a dictionary)\n"
             << "file size:  " << ( src.size >> 20 ) << " MB\n";
   return 0;
}

// Trains a zstd dictionary on blocks picked at random from [from, to] (read only).
int train( const bpo::variables_map& o )
{
   source_log src( o["input"].as< std::string >() );
   const uint64_t from = o["from"].as< uint64_t >();
   const uint64_t to = std::min< uint64_t >( o["to"].as< uint64_t >(), src.blocks - 1 );
   const size_t dict_size = o["dict-size"].as< size_t >();
   const size_t budget = o["samples-mb"].as< size_t >() << 20;
   FC_ASSERT( from >= 1 && from < to, "bad range" );

   std::mt19937_64 rng( 12345 ); // fixed seed: the same range gives the same dictionary
   std::vector< char > samples;
   std::vector< size_t > sizes;
   samples.reserve( budget + ( 1 << 20 ) );
   while( samples.size() < budget )
   {
      const uint64_t n = from + rng() % ( to - from + 1 );
      auto raw = source_raw( src, n );
      samples.insert( samples.end(), raw.begin(), raw.end() );
      sizes.push_back( raw.size() );
   }
   std::cerr << "training on " << sizes.size() << " blocks (" << ( samples.size() >> 20 ) << " MB) from " << from << " .. " << to << "\n";

   std::vector< char > dict( dict_size );
   ZDICT_fastCover_params_t params;
   memset( &params, 0, sizeof( params ) );
   params.nbThreads = std::max( 1, o["threads"].as< int >() );
   params.splitPoint = 0.75;
   params.accel = 1;
   params.steps = 40;
   params.zParams.compressionLevel = 3;
   const size_t r = ZDICT_optimizeTrainFromBuffer_fastCover( dict.data(), dict.size(), samples.data(), sizes.data(),
                                                             unsigned( sizes.size() ), &params );
   FC_ASSERT( !ZDICT_isError( r ), "dictionary training failed: ${e}", ("e", ZDICT_getErrorName( r )) );
   std::ofstream out( o["out"].as< std::string >(), std::ios::binary );
   out.write( dict.data(), r );
   std::cerr << "wrote " << r << " bytes (k=" << params.k << " d=" << params.d << ")\n";
   return 0;
}

// How much the blocks of a range shrink with the current settings (every step-th block, nothing is written).
int estimate( const bpo::variables_map& o )
{
   source_log src( o["input"].as< std::string >() );
   const uint64_t from = o["from"].as< uint64_t >();
   const uint64_t to = std::min< uint64_t >( o["to"].as< uint64_t >(), src.blocks - 1 );
   const uint64_t step = std::max< uint64_t >( 1, o["step"].as< uint64_t >() );
   const int level = o["level"].as< int >();
   const bool use_dict = !o.count( "no-dictionary" );
   uint64_t raw_bytes = 0, packed_bytes = 0, blocks = 0, stored_plain = 0;
   std::vector< char > packed;
   const auto t0 = std::chrono::steady_clock::now();
   for( uint64_t n = from; n <= to; n += step )
   {
      auto raw = source_raw( src, n );
      raw_bytes += raw.size();
      fc::optional< uint8_t > dict;
      if( use_dict ) dict = block_log_codec::dictionary_for_block( uint32_t( n ) );
      if( block_log_codec::compress( raw.data(), raw.size(), level, dict, packed ) )
         packed_bytes += packed.size();
      else
      {
         packed_bytes += raw.size();
         ++stored_plain;
      }
      ++blocks;
   }
   const double secs = std::chrono::duration< double >( std::chrono::steady_clock::now() - t0 ).count();
   std::cout << from << " .. " << to << " every " << step << ": " << blocks << " blocks, "
             << raw_bytes / blocks << " bytes each -> " << ( 100.0 * packed_bytes / raw_bytes ) << "% ("
             << stored_plain << " kept plain) at level " << level << ( use_dict ? " with dictionaries" : "" )
             << "   " << uint64_t( blocks / std::max( secs, 0.001 ) ) << " blocks/s\n";
   return 0;
}

} // anonymous

int main( int argc, char** argv )
{
   try
   {
      if( argc < 2 )
      {
         std::cerr << "usage: compress_block_log convert|verify|stats --input FILE [options]\n";
         return 2;
      }
      const std::string command = argv[1];
      bpo::options_description od( "options" );
      od.add_options()
         ( "input", bpo::value< std::string >()->required(), "block log to read" )
         ( "output", bpo::value< std::string >(), "convert: block log to write (it is created, or continued if it already holds blocks)" )
         ( "level", bpo::value< int >()->default_value( 3 ), "convert: zstd level 1-22, 0 writes plain blocks" )
         ( "no-dictionary", "convert: compress without the built-in dictionaries" )
         ( "threads", bpo::value< int >()->default_value( 4 ), "worker threads" )
         ( "from", bpo::value< uint64_t >(), "first block" )
         ( "to", bpo::value< uint64_t >(), "last block" )
         ( "include-last", "convert: also convert the last block of the index (only when the node is stopped)" )
         ( "reference", bpo::value< std::string >(), "verify: another block log whose blocks must be identical" )
         ( "out", bpo::value< std::string >(), "train: file for the dictionary" )
         ( "samples-mb", bpo::value< size_t >()->default_value( 40 ), "train: megabytes of blocks to learn from" )
         ( "dict-size", bpo::value< size_t >()->default_value( 220000 ), "train: size of the dictionary in bytes" )
         ( "step", bpo::value< uint64_t >()->default_value( 50 ), "estimate: look at every step-th block" );
      bpo::variables_map vm;
      bpo::store( bpo::parse_command_line( argc - 1, argv + 1, od ), vm );
      bpo::notify( vm );

      if( command == "convert" ) { FC_ASSERT( vm.count( "output" ), "--output is required" ); return convert( vm ); }
      if( command == "verify" )  return verify( vm );
      if( command == "stats" )   return stats( vm );
      if( command == "train" )   { FC_ASSERT( vm.count( "out" ) && vm.count( "from" ) && vm.count( "to" ), "--out, --from and --to are required" ); return train( vm ); }
      if( command == "estimate" ) { FC_ASSERT( vm.count( "from" ) && vm.count( "to" ), "--from and --to are required" ); return estimate( vm ); }
      std::cerr << "unknown command " << command << "\n";
      return 2;
   }
   catch( const fc::exception& e )
   {
      std::cerr << e.to_detail_string() << "\n";
      return 1;
   }
   catch( const std::exception& e )
   {
      std::cerr << e.what() << "\n";
      return 1;
   }
}
