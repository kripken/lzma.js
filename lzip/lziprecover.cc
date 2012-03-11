/*  Lziprecover - Data recovery tool for lzip compressed files
    Copyright (C) 2008, 2009, 2010, 2011 Antonio Diaz Diaz.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
/*
    Return values: 0 for a normal exit, 1 for environmental problems
    (file not found, invalid flags, I/O errors, etc), 2 to indicate a
    corrupt or invalid input file, 3 for an internal consistency error
    (eg, bug) which caused lziprecover to panic.
*/

#define _FILE_OFFSET_BITS 64

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#if defined(__MSVCRT__)
#define S_IRGRP 0
#define S_IWGRP 0
#define S_IROTH 0
#define S_IWOTH 0
#endif

#include "arg_parser.h"
#include "lzip.h"
#include "decoder.h"

#if CHAR_BIT != 8
#error "Environments where CHAR_BIT != 8 are not supported."
#endif

#ifndef LLONG_MAX
#define LLONG_MAX  0x7FFFFFFFFFFFFFFFLL
#endif
#ifndef LLONG_MIN
#define LLONG_MIN  (-LLONG_MAX - 1LL)
#endif
#ifndef ULLONG_MAX
#define ULLONG_MAX 0xFFFFFFFFFFFFFFFFULL
#endif


namespace {

const char * const Program_name = "Lziprecover";
const char * const program_name = "lziprecover";
const char * const program_year = "2011";
const char * invocation_name = 0;

#ifdef O_BINARY
const int o_binary = O_BINARY;
#else
const int o_binary = 0;
#endif

int verbosity = 0;


class Block
  {
  long long pos_, size_;		// pos + size <= LLONG_MAX

public:
  Block( const long long p, const long long s ) throw()
    : pos_( p ), size_( s ) {}

  long long pos() const throw() { return pos_; }
  long long size() const throw() { return size_; }
  long long end() const throw() { return pos_ + size_; }

  void pos( const long long p ) throw() { pos_ = p; }
  void size( const long long s ) throw() { size_ = s; }
  void shift( Block & b ) throw() { ++size_; ++b.pos_; --b.size_; }
  };


void show_help() throw()
  {
  std::printf( "%s - Data recovery tool for lzip compressed files.\n", Program_name );
  std::printf( "\nUsage: %s [options] [files]\n", invocation_name );
  std::printf( "\nOptions:\n" );
  std::printf( "  -h, --help                 display this help and exit\n" );
  std::printf( "  -V, --version              output version information and exit\n" );
//  std::printf( "  -c, --create-recover-file  create a recover file\n" );
  std::printf( "  -f, --force                overwrite existing output files\n" );
  std::printf( "  -m, --merge                correct errors in file using several copies\n" );
  std::printf( "  -o, --output=<file>        place the output into <file>\n" );
  std::printf( "  -q, --quiet                suppress all messages\n" );
//  std::printf( "  -r, --recover              correct errors in file using a recover file\n" );
  std::printf( "  -R, --repair               try to repair a small error in file\n" );
  std::printf( "  -s, --split                split a multimember file in single-member files\n" );
//  std::printf( "  -u, --update               convert file from version 0 to version 1\n" );
  std::printf( "  -v, --verbose              be verbose (a 2nd -v gives more)\n" );
  std::printf( "\nReport bugs to lzip-bug@nongnu.org\n");
  std::printf( "Lzip home page: http://www.nongnu.org/lzip/lzip.html\n" );
  }


void show_version() throw()
  {
  std::printf( "%s %s\n", Program_name, PROGVERSION );
  std::printf( "Copyright (C) %s Antonio Diaz Diaz.\n", program_year );
  std::printf( "License GPLv3+: GNU GPL version 3 or later <http://gnu.org/licenses/gpl.html>\n" );
  std::printf( "This is free software: you are free to change and redistribute it.\n" );
  std::printf( "There is NO WARRANTY, to the extent permitted by law.\n" );
  }


int open_instream( const std::string & input_filename ) throw()
  {
  int infd = open( input_filename.c_str(), O_RDONLY | o_binary );
  if( infd < 0 )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "%s: Can't open input file `%s': %s.\n",
                    program_name, input_filename.c_str(), std::strerror( errno ) );
    }
  else
    {
    struct stat in_stats;
    const int i = fstat( infd, &in_stats );
    if( i < 0 || !S_ISREG( in_stats.st_mode ) )
      {
      if( verbosity >= 0 )
        std::fprintf( stderr, "%s: Input file `%s' is not a regular file.\n",
                      program_name, input_filename.c_str() );
      close( infd );
      infd = -1;
      }
    }
  return infd;
  }


int open_outstream( const std::string & output_filename,
                    const bool force ) throw()
  {
  int flags = O_CREAT | O_RDWR | o_binary;
  if( force ) flags |= O_TRUNC; else flags |= O_EXCL;

  int outfd = open( output_filename.c_str(), flags,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH );
  if( outfd < 0 && verbosity >= 0 )
    {
    if( errno == EEXIST )
      std::fprintf( stderr, "%s: Output file `%s' already exists."
                            " Use `--force' to overwrite it.\n",
                    program_name, output_filename.c_str() );
    else
      std::fprintf( stderr, "%s: Can't create output file `%s': %s.\n",
                    program_name, output_filename.c_str(), std::strerror( errno ) );
    }
  return outfd;
  }


bool verify_header( const File_header & header )
  {
  if( !header.verify_magic() )
    {
    show_error( "Bad magic number (file not in lzip format)." );
    return false;
    }
  if( header.version() == 0 )
    {
    show_error( "Version 0 member format can't be recovered." );
    return false;
    }
  if( header.version() != 1 )
    {
    if( verbosity >= 0 )
      std::fprintf( stderr, "Version %d member format not supported.\n",
                    header.version() );
    return false;
    }
  return true;
  }


bool verify_single_member( const int fd, const long long file_size )
  {
  File_header header;
  if( lseek( fd, 0, SEEK_SET ) < 0 ||
      readblock( fd, header.data, File_header::size ) != File_header::size )
    { show_error( "Error reading member header", errno ); return false; }
  if( !verify_header( header ) ) return false;

  File_trailer trailer;
  if( lseek( fd, -File_trailer::size(), SEEK_END ) < 0 ||
      readblock( fd, trailer.data, File_trailer::size() ) != File_trailer::size() )
    { show_error( "Error reading member trailer", errno ); return false; }
  const long long member_size = trailer.member_size();
  if( member_size != file_size )
    {
    if( member_size < file_size &&
        lseek( fd, -member_size, SEEK_END ) > 0 &&
        readblock( fd, header.data, File_header::size ) == File_header::size &&
        verify_header( header ) )
      show_error( "Input file has more than 1 member. Split it first." );
    else
      show_error( "Member size in input file trailer is corrupt." );
    return false;
    }
  return true;
  }


bool try_decompress( const int fd, const long long file_size,
                     long long * failure_pos = 0 )
  {
  try {
    Range_decoder rdec( fd );
    File_header header;
    rdec.reset_member_position();
    for( int i = 0; i < File_header::size; ++i )
      header.data[i] = rdec.get_byte();
    if( !rdec.finished() &&			// End Of File
        header.verify_magic() &&
        header.version() == 1 &&
        header.dictionary_size() >= min_dictionary_size &&
          header.dictionary_size() <= max_dictionary_size )
      {
      LZ_decoder decoder( header, rdec, -1 );
      std::vector< std::string > dummy_filenames;
      Pretty_print dummy( dummy_filenames, -1 );

      if( decoder.decode_member( dummy ) == 0 &&
          rdec.member_position() == file_size ) return true;
      if( failure_pos ) *failure_pos = rdec.member_position();
      }
    }
  catch( std::bad_alloc )
    {
    show_error( "Not enough memory. Find a machine with more memory." );
    std::exit( 1 );
    }
  catch( Error e ) {}
  return false;
  }


bool copy_and_diff_file( const std::vector< int > & infd_vector,
                         const int outfd, std::vector< Block > & block_vector )
  {
  const int buffer_size = 65536;
  std::vector< uint8_t * > buffer_vector( infd_vector.size() );
  for( unsigned int i = 0; i < infd_vector.size(); ++i )
    buffer_vector[i] = new uint8_t[buffer_size];
  Block b( 0, 0 );
  long long partial_pos = 0;
  int equal_bytes = 0;
  bool error = false;

  while( !error )
    {
    const int rd = readblock( infd_vector[0], buffer_vector[0], buffer_size );
    if( rd != buffer_size && errno )
      { show_error( "Error reading input file", errno ); error = true; }
    if( rd > 0 )
      {
      for( unsigned int i = 1; i < infd_vector.size(); ++i )
        if( readblock( infd_vector[i], buffer_vector[i], rd ) != rd )
          { show_error( "Error reading input file", errno ); error = true; }
      const int wr = writeblock( outfd, buffer_vector[0], rd );
      if( wr != rd )
        { show_error( "Error writing output file", errno ); error = true; }
      for( int i = 0; i < rd; ++i )
        {
        while( i < rd && b.pos() == 0 )
          {
          for( unsigned int j = 1; j < infd_vector.size(); ++j )
            if( buffer_vector[0][i] != buffer_vector[j][i] )
              { b.pos( partial_pos + i ); break; }	// begin block
          ++i;
          }
        while( i < rd && b.pos() > 0 )
          {
          ++equal_bytes;
          for( unsigned int j = 1; j < infd_vector.size(); ++j )
            if( buffer_vector[0][i] != buffer_vector[j][i] )
              { equal_bytes = 0; break; }
          if( equal_bytes >= 2 )			// end block
            {
            b.size( partial_pos + i - ( equal_bytes - 1 ) - b.pos() );
            block_vector.push_back( b );
            b.pos( 0 );
            equal_bytes = 0;
            }
          ++i;
          }
        }
      partial_pos += rd;
      }
    if( rd < buffer_size ) break;			// EOF
    }
  if( b.pos() > 0 )					// finish last block
    {
    b.size( partial_pos - b.pos() );
    block_vector.push_back( b );
    }
  for( unsigned int i = 0; i < infd_vector.size(); ++i )
    delete[] buffer_vector[i];
  return !error;
  }


bool copy_file( const int infd, const int outfd,
                const long long size = LLONG_MAX )
  {
  long long rest = size;
  const int buffer_size = 65536;
  uint8_t * const buffer = new uint8_t[buffer_size];
  bool error = false;

  while( !error )
    {
    const int block_size = std::min( (long long)buffer_size, rest );
    if( block_size <= 0 ) break;
    const int rd = readblock( infd, buffer, block_size );
    if( rd != block_size && errno )
      { show_error( "Error reading input file", errno ); error = true; }
    if( rd > 0 )
      {
      const int wr = writeblock( outfd, buffer, rd );
      if( wr != rd )
        { show_error( "Error writing output file", errno ); error = true; }
      rest -= rd;
      }
    if( rd < block_size ) break;			// EOF
    }
  delete[] buffer;
  return !error;
  }


std::string insert_fixed( std::string name ) throw()
  {
  if( name.size() > 4 && name.compare( name.size() - 4, 4, ".tlz" ) == 0 )
    name.insert( name.size() - 4, "_fixed" );
  else if( name.size() > 3 && name.compare( name.size() - 3, 3, ".lz" ) == 0 )
    name.insert( name.size() - 3, "_fixed" );
  else name += "_fixed.lz";
  return name;
  }


int ipow( const unsigned int base, const unsigned int exponent ) throw()
  {
  int result = 1;
  for( unsigned int i = 0; i < exponent; ++i )
    {
    if( INT_MAX / base >= (unsigned int)result ) result *= base;
    else { result = INT_MAX; break; }
    }
  return result;
  }


int merge_files( const std::vector< std::string > & filenames,
                 const std::string & output_filename, const bool force )
  {
  std::vector< int > infd_vector( filenames.size() );
  for( unsigned int i = 0; i < filenames.size(); ++i )
    {
    infd_vector[i] = open_instream( filenames[i] );
    if( infd_vector[i] < 0 ) return 1;
    }
  long long isize = 0;
  for( unsigned int i = 0; i < filenames.size(); ++i )
    {
    const long long tmp = lseek( infd_vector[i], 0, SEEK_END );
    if( tmp < 0 )
      {
      if( verbosity >= 0 )
        std::fprintf( stderr, "File `%s' is not seekable.\n", filenames[i].c_str() );
      return 1;
      }
    if( i == 0 ) isize = tmp;
    else if( isize != tmp )
      { show_error( "Sizes of input files are different." ); return 1; }
    }
  if( isize < 36 )
    { show_error( "Input file is too short." ); return 2; }
  for( unsigned int i = 0; i < filenames.size(); ++i )
    if( !verify_single_member( infd_vector[i], isize ) )
      return 2;
  for( unsigned int i = 0; i < filenames.size(); ++i )
    if( lseek( infd_vector[i], 0, SEEK_SET ) < 0 )
      { show_error( "Seek error in input file", errno ); return 1; }
  for( unsigned int i = 0; i < filenames.size(); ++i )
    if( try_decompress( infd_vector[i], isize ) )
      {
      if( verbosity >= 1 )
        std::printf( "File `%s' has no errors. Recovery is not needed.\n",
                     filenames[i].c_str() );
      return 0;
      }

  const int outfd = open_outstream( output_filename, force );
  if( outfd < 0 ) return 1;
  for( unsigned int i = 0; i < filenames.size(); ++i )
    if( lseek( infd_vector[i], 0, SEEK_SET ) < 0 )
      { show_error( "Seek error in input file", errno ); return 1; }

  // vector of data blocks differing among the copies of the input file.
  std::vector< Block > block_vector;
  if( !copy_and_diff_file( infd_vector, outfd, block_vector ) ) return 1;

  if( !block_vector.size() )
    { show_error( "Input files are identical. Recovery is not possible." );
      return 1; }

  const bool single_block = ( block_vector.size() == 1 );
  if( single_block && block_vector[0].size() < 2 )
    { show_error( "Input files have the same byte damaged."
                  " Try repairing one of them." );
      return 1; }

  if( ipow( filenames.size(), block_vector.size() ) >= INT_MAX ||
      ( single_block &&
        ipow( filenames.size(), 2 ) >= INT_MAX / block_vector[0].size() ) )
    { show_error( "Input files are too damaged. Recovery is not possible." );
      return 1; }

  const int shifts = ( single_block ? block_vector[0].size() - 1 : 1 );
  if( single_block )
    {
    Block b( block_vector[0].pos() + 1, block_vector[0].size() - 1 );
    block_vector[0].size( 1 );
    block_vector.push_back( b );
    }

  const int base_variations = ipow( filenames.size(), block_vector.size() );
  const int variations = ( base_variations * shifts ) - 2;
  bool done = false;
  for( int var = 1; var <= variations; ++var )
    {
    if( verbosity >= 1 )
      {
      std::printf( "Trying variation %d of %d \r", var, variations );
      std::fflush( stdout );
      }
    int tmp = var;
    for( unsigned int i = 0; i < block_vector.size(); ++i )
      {
      const int infd = infd_vector[tmp % filenames.size()];
      tmp /= filenames.size();
      if( lseek( infd, block_vector[i].pos(), SEEK_SET ) < 0 ||
          lseek( outfd, block_vector[i].pos(), SEEK_SET ) < 0 ||
          !copy_file( infd, outfd, block_vector[i].size() ) )
        { show_error( "Error reading output file", errno ); return 1; }
      }
    if( lseek( outfd, 0, SEEK_SET ) < 0 )
      { show_error( "Seek error in output file", errno ); return 1; }
    if( try_decompress( outfd, isize ) )
      { done = true; break; }
    if( var % base_variations == 0 ) block_vector[0].shift( block_vector[1] );
    }
  if( verbosity >= 1 ) std::printf( "\n" );

  if( close( outfd ) != 0 )
    { show_error( "Error closing output file", errno ); return 1; }
  if( done )
    {
    if( verbosity >= 1 )
      std::printf( "Input files merged successfully.\n" );
    return 0;
    }
  else
    {
    std::remove( output_filename.c_str() );
    show_error( "Some error areas overlap. Can't recover input file." );
    return 2;
    }
  }


int repair_file( const std::string & input_filename,
                 const std::string & output_filename, const bool force )
  {
  const int infd = open_instream( input_filename );
  if( infd < 0 ) return 1;
  const long long isize = lseek( infd, 0, SEEK_END );
  if( isize < 0 )
    { show_error( "Input file is not seekable", errno ); return 1; }
  if( isize < 36 )
    { show_error( "Input file is too short." ); return 2; }
  if( !verify_single_member( infd, isize ) ) return 2;
  if( lseek( infd, 0, SEEK_SET ) < 0 )
    { show_error( "Seek error in input file", errno ); return 1; }
  long long failure_pos = 0;
  if( try_decompress( infd, isize, &failure_pos ) )
    {
    if( verbosity >= 1 )
      std::printf( "Input file has no errors. Recovery is not needed.\n" );
    return 0;
    }
  if( failure_pos >= isize - 8 ) failure_pos = isize - 8 - 1;
  if( failure_pos < File_header::size )
    { show_error( "Can't repair error in input file." ); return 2; }

  const int outfd = open_outstream( output_filename, force );
  if( outfd < 0 ) { close( infd ); return 1; }
  if( lseek( infd, 0, SEEK_SET ) < 0 )
    { show_error( "Seek error in input file", errno ); return 1; }
  if( !copy_file( infd, outfd ) ) return 1;

  const long long min_pos =
    std::max( (long long)File_header::size, failure_pos - 1000 );
  bool done = false;
  for( long long pos = failure_pos; pos >= min_pos; --pos )
    {
    if( verbosity >= 1 )
      {
      std::printf( "Trying position %lld \r", pos );
      std::fflush( stdout );
      }
    uint8_t byte;
    if( lseek( outfd, pos, SEEK_SET ) < 0 ||
        readblock( outfd, &byte, 1 ) != 1 )
      { show_error( "Error reading output file", errno ); return 1; }
    for( int i = 0; i < 255; ++i )
      {
      ++byte;
      if( lseek( outfd, pos, SEEK_SET ) < 0 ||
          writeblock( outfd, &byte, 1 ) != 1 ||
          lseek( outfd, 0, SEEK_SET ) < 0 )
        { show_error( "Error writing output file", errno ); return 1; }
      if( try_decompress( outfd, isize ) )
        { done = true; break; }
      }
    if( done ) break;
    ++byte;
    if( lseek( outfd, pos, SEEK_SET ) < 0 ||
        writeblock( outfd, &byte, 1 ) != 1 )
      { show_error( "Error writing output file", errno ); return 1; }
    }
  if( verbosity >= 1 ) std::printf( "\n" );

  if( close( outfd ) != 0 )
    { show_error( "Error closing output file", errno ); return 1; }
  if( done )
    {
    if( verbosity >= 1 )
      std::printf( "Copy of input file repaired successfully.\n" );
    return 0;
    }
  else
    {
    std::remove( output_filename.c_str() );
    show_error( "Error is larger than 1 byte. Can't repair input file." );
    return 2;
    }
  }


bool next_filename( std::string & output_filename )
  {
  for( int i = 7; i >= 3; --i )			// "rec00001"
    {
    if( output_filename[i] < '9' ) { ++output_filename[i]; return true; }
    else output_filename[i] = '0';
    }
  return false;
  }


int do_split_file( const std::string & input_filename, uint8_t * & base_buffer,
                   const std::string & default_output_filename, const bool force )
  {
  const int hsize = File_header::size;
  const int tsize = File_trailer::size();
  const int buffer_size = 65536;
  const int base_buffer_size = tsize + buffer_size + hsize;
  base_buffer = new uint8_t[base_buffer_size];
  uint8_t * const buffer = base_buffer + tsize;

  const int infd = open_instream( input_filename );
  if( infd < 0 ) return 1;
  int size = readblock( infd, buffer, buffer_size + hsize ) - hsize;
  bool at_stream_end = ( size < buffer_size );
  if( size != buffer_size && errno )
    { show_error( "Read error", errno ); return 1; }
  if( size <= tsize )
    { show_error( "Input file is too short." ); return 2; }
  File_header header;
  for( int i = 0; i < File_header::size; ++i )
    header.data[i] = buffer[i];
  if( !verify_header( header ) ) return 2;

  std::string output_filename( "rec00001" );
  output_filename += default_output_filename;
  int outfd = open_outstream( output_filename, force );
  if( outfd < 0 ) { close( infd ); return 1; }

  long long partial_member_size = 0;
  while( true )
    {
    int pos = 0;
    for( int newpos = 1; newpos <= size; ++newpos )
      if( buffer[newpos] == magic_string[0] &&
          buffer[newpos+1] == magic_string[1] &&
          buffer[newpos+2] == magic_string[2] &&
          buffer[newpos+3] == magic_string[3] )
        {
        long long member_size = 0;
        for( int i = 1; i <= 8; ++i )
          { member_size <<= 8; member_size += base_buffer[tsize+newpos-i]; }
        if( partial_member_size + newpos - pos == member_size )
          {						// header found
          const int wr = writeblock( outfd, buffer + pos, newpos - pos );
          if( wr != newpos - pos )
            { show_error( "Write error", errno ); return 1; }
          if( close( outfd ) != 0 )
            { show_error( "Error closing output file", errno ); return 1; }
          if( !next_filename( output_filename ) )
            { show_error( "Too many members in file." ); close( infd ); return 1; }
          outfd = open_outstream( output_filename, force );
          if( outfd < 0 ) { close( infd ); return 1; }
          partial_member_size = 0;
          pos = newpos;
          }
        }

    if( at_stream_end )
      {
      const int wr = writeblock( outfd, buffer + pos, size + hsize - pos );
      if( wr != size + hsize - pos )
        { show_error( "Write error", errno ); return 1; }
      break;
      }
    if( pos < buffer_size )
      {
      partial_member_size += buffer_size - pos;
      const int wr = writeblock( outfd, buffer + pos, buffer_size - pos );
      if( wr != buffer_size - pos )
        { show_error( "Write error", errno ); return 1; }
      }
    std::memcpy( base_buffer, base_buffer + buffer_size, tsize + hsize );
    size = readblock( infd, buffer + hsize, buffer_size );
    at_stream_end = ( size < buffer_size );
    if( size != buffer_size && errno )
      { show_error( "Read error", errno ); return 1; }
    }
  close( infd );
  if( close( outfd ) != 0 )
    { show_error( "Error closing output file", errno ); return 1; }
  return 0;
  }


int split_file( const std::string & input_filename,
                const std::string & default_output_filename, const bool force )
  {
  uint8_t * base_buffer;
  const int retval = do_split_file( input_filename, base_buffer,
                                    default_output_filename, force );
  delete[] base_buffer;
  return retval;
  }

} // end namespace


void show_error( const char * const msg, const int errcode, const bool help ) throw()
  {
  if( verbosity >= 0 )
    {
    if( msg && msg[0] )
      {
      std::fprintf( stderr, "%s: %s", program_name, msg );
      if( errcode > 0 )
        std::fprintf( stderr, ": %s", std::strerror( errcode ) );
      std::fprintf( stderr, "\n" );
      }
    if( help && invocation_name && invocation_name[0] )
      std::fprintf( stderr, "Try `%s --help' for more information.\n",
                    invocation_name );
    }
  }


void internal_error( const char * const msg )
  {
  if( verbosity >= 0 )
    std::fprintf( stderr, "%s: internal error: %s.\n", program_name, msg );
  std::exit( 3 );
  }


int main( const int argc, const char * const argv[] )
  {
  enum Mode
    { m_none, m_create, m_merge, m_recover, m_repair, m_split, m_update };
  Mode program_mode = m_none;
  bool force = false;
  std::string default_output_filename;
  invocation_name = argv[0];

  const Arg_parser::Option options[] =
    {
    { 'f', "force",           Arg_parser::no  },
    { 'h', "help",            Arg_parser::no  },
    { 'm', "merge",           Arg_parser::no  },
    { 'o', "output",          Arg_parser::yes },
    { 'q', "quiet",           Arg_parser::no  },
    { 'R', "repair",          Arg_parser::no  },
    { 's', "split",           Arg_parser::no  },
    { 'v', "verbose",         Arg_parser::no  },
    { 'V', "version",         Arg_parser::no  },
    {  0 ,  0,                Arg_parser::no  } };

  Arg_parser parser( argc, argv, options );
  if( parser.error().size() )				// bad option
    { show_error( parser.error().c_str(), 0, true ); return 1; }

  int argind = 0;
  for( ; argind < parser.arguments(); ++argind )
    {
    const int code = parser.code( argind );
    if( !code ) break;					// no more options
    switch( code )
      {
      case 'f': force = true; break;
      case 'h': show_help(); return 0;
      case 'm': program_mode = m_merge; break;
      case 'o': default_output_filename = parser.argument( argind ); break;
      case 'q': verbosity = -1; break;
      case 'R': program_mode = m_repair; break;
      case 's': program_mode = m_split; break;
      case 'v': if( verbosity < 4 ) ++verbosity; break;
      case 'V': show_version(); return 0;
      default : internal_error( "uncaught option" );
      }
    }

  if( program_mode == m_merge )
    {
    std::vector< std::string > filenames;
    for( ; argind < parser.arguments(); ++argind )
      filenames.push_back( parser.argument( argind ) );
    if( filenames.size() < 2 )
      { show_error( "You must specify at least 2 files.", 0, true ); return 1; }
    if( !default_output_filename.size() )
      default_output_filename = insert_fixed( filenames[0] );
    return merge_files( filenames, default_output_filename, force );
    }

  if( argind + 1 != parser.arguments() )
    { show_error( "You must specify exactly 1 file.", 0, true ); return 1; }

  if( program_mode == m_repair )
    {
    if( !default_output_filename.size() )
      default_output_filename = insert_fixed( parser.argument( argind ) );
    return repair_file( parser.argument( argind ), default_output_filename, force );
    }

  if( program_mode == m_split )
    {
    if( !default_output_filename.size() )
      default_output_filename = parser.argument( argind );
    return split_file( parser.argument( argind ), default_output_filename, force );
    }

  show_error( "You must specify the operation to be performed on file.",
              0, true );
  return 1;
  }
