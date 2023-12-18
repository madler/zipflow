/*
  zipflow version 1.4, 13 December 2023

  Copyright (C) 2022-2023 Mark Adler

  This software is provided 'as-is', without any express or implied
  warranty.  In no event will the author be held liable for any damages
  arising from the use of this software.

  Permission is granted to anyone to use this software for any purpose,
  including commercial applications, and to alter it and redistribute it
  freely, subject to the following restrictions:

  1. The origin of this software must not be misrepresented; you must not
     claim that you wrote the original software. If you use this software
     in a product, an acknowledgment in the product documentation would be
     appreciated but is not required.
  2. Altered source versions must be plainly marked as such, and must not be
     misrepresented as being the original software.
  3. This notice may not be removed or altered from any source distribution.

  Mark Adler    madler@alumni.caltech.edu
 */

/* Version history:
   1.0  15 Aug 2022  First version
   1.1  24 Aug 2022  Portability and comment improvements
   1.2  30 Sep 2022  Add Windows support for attributes and directory traversal
                     Only include timestamps in central headers
   1.3   3 Jan 2023  Interpret file and directory names as UTF-8 in Windows
                     Fix bugs in zip_data() for large lengths
   1.4  13 Dec 2023  Fix bug in zip_data() due to uninitialized count
 */

// zipflow is a streaming zipper. Names of files and directories, or metadata
// and file data are provided to zip. The resulting zip file is streamed out
// without seeking. The Zip64 format is used as needed. When compiling, link
// with zlib (-lz).

// Basic usage:
//
//      ZIP *zip = zip_open(outfile, level);
//      zip_entry(zip, name_1);
//      zip_entry(zip, name_2);
//      ...
//      zip_entry(zip, name_n);
//      zip_close(zip);
//
// where outfile is an open FILE * in binary mode, level is the compression
// level (-1..9), and name_i are the names of files and/or directories to zip.
//
// To write a zip file entry directly, providing the data and metadata:
//
//      ZIP *zip = zip_open(outfile, level);
//      time_t t = time(NULL);
//      zip_meta(zip, "path-in-zip-file", 3, 0644, t, t);
//      zip_data(zip, data_1, len_1, 0);
//      zip_data(zip, data_2, len_2, 0);
//      ...
//      zip_data(zip, data_n, len_n, 1);
//      zip_close(zip);
//
// where the entry data are the blocks data_i, each with len_i bytes. See below
// for more details on the arguments to zip_meta(). The last call of zip_data()
// has a 1 in the last argument to complete the entry. Multiple calls to
// zip_entry() and zip_meta()/zip_data() can be mixed as desired.
//
// Instead of writing to a file, an output function can be registered by using
// zip_pipe() instead of zip_open():
//
//      int put(void *handle, void const *ptr, size_t len) {
//          return fwrite(ptr, 1, len, (FILE *)handle) < len;
//      }
//      ZIP *zip = zip_pipe(stdout, put, level);
//
// Of course, that also writes to a file, but you get the idea. It could,
// instead, write to a communication channel, or do other processing on the
// streaming zip file data.
//
// Warning and error messages can optionally be captured by a registered
// function. For example:
//
//      void log(void *out, char *msg) {
//          fprintf((FILE *)out, "zip issue: %s\n", msg);
//          free(msg);
//      }
//
//      ZIP *zip = zip_open(stdout, level);
//      FILE *dump = fopen("dump.log", "w");
//      zip_log(zip, dump, log);
//      ...
//      zip_close(zip);
//      fclose(dump);
//
// will write all messages to dump.log instead of stderr.

// Motivation for zipflow: Normally, zip file processing requires random access
// to the zip file on mass storage or in memory. This streaming zipper does not
// require such random access, and does not require that the entire zip file be
// in mass storage or memory at any time. No matter how large the input files
// are or how large the resulting zip file is, the amount of memory used by
// this code for the input and output data, as well as for the compression
// process, is small and constant, less than 800 KiB. Additional memory is used
// to save metadata on the files written to the zip file, proportional to the
// number of files. This is required to be able to write the zip directory at
// the end of the zip file. That amount of memory is equal to a small constant
// plus the average length of a file name, multiplied by the number of entries
// in the resulting zip file. The small constant is 64 to 72 bytes plus the
// null termination and allocation overhead for the file name.

#include <stdio.h>
#include <stdint.h>
typedef void ZIP;           // opaque structure for zip streaming operations

// Return the zip state to write a zip file to out. If out is file, it is
// assumed that the current write position is zero. On systems where it
// matters, out must be in binary mode. level is the compression level for
// deflate, in the range -1..9 as defined in zlib.h. A pointer to the zip state
// is returned on success. NULL is returned if out is NULL or level is out of
// range.
ZIP *zip_open(FILE *out, int level);

// Like zip_open(), but instead of a FILE *, register the function put() for
// the streaming zip file output. put() accepts the len bytes at ptr. If ptr is
// NULL, then the streaming has completed, and put() may flush the output.
// put() returns 0 on success, or 1 to abort the streaming. If put() returns 1,
// it will not be called again for that ZIP * instance. No error message is
// issued in that case. A pointer to the zip state is returned on success. NULL
// is returned if put is NULL or level is out of range.
ZIP *zip_pipe(void *handle,
              int (*put)(void *handle, void const *ptr, size_t len),
              int level);

// Register the function log() to intercept warning and error messages. msg is
// an allocated zero-terminated string containing the message. The user is
// responsible for freeing the allocation. hook is passed to the log() function
// on each call, which can point to a user-defined and supplied structure with
// information used to process the message. zip_log() can be called at any time
// after zip_open() and before zip_close(). The previous log() function can be
// unregistered by passing NULL for the function pointer. When not intercepted,
// the error messages are printed on stderr with an added "zipflow: " prefix
// and new line suffix. On success, 0 is returned. If zip is not valid, then -1
// is returned.
int zip_log(ZIP *zip, void *hook, void (*log)(void *hook, char *msg));

// Add an entry to the zip file with the file path, or entries to the zip file
// with any files contained at any level in the directory path. On success, 0
// is returned. If zip is not valid, then -1 is returned. If there is a write
// error, then 1 is returned. In that event, no further writing will be
// attempted on this stream, and the only viable action is zip_close().
int zip_entry(ZIP *zip, char const *path);

// Prepare to write a new zip entry by providing the metadata for the entry:
// the name path and the operating system os, followed by operating-system
// specific parameters. path is limited by the zip format to no more than 65535
// bytes in length. os must be 3 for Unix attributes, or 10 for Windows
// attributes. See the commented prototypes below for the types. The next call
// must be zip_data() to write the entry data. On success, 0 is returned. If
// zip, path, or os are invalid, -1 is returned. Nothing is written to the zip
// file by this function, so there is no possibility of a new write error.
int zip_meta(ZIP *zip, char const *path, int os, ...);
// Unix:
//      int zip_meta(ZIP *zip, char const *path, 3, unsigned mode,
//                   uint32_t atime, uint32_t mtime);
// Windows:
//      int zip_meta(ZIP *zip, char const *path, 10, uint32_t attr,
//                   uint64_t ctime, uint64_t atime, uint64_t mtime);

// Compress and write the len bytes at data to the current entry in the zip
// file. Complete the entry if last is true. zip_data() can only be called
// after zip_meta(), or after a non-last zip_data() call. On success, 0 is
// returned. If zip is invalid, -1 is returned. If there is a write error, 1 is
// returned.
int zip_data(ZIP *zip, void const *data, size_t len, int last);

// Complete the zip file by writing the zip directory at the end. Close the zip
// object, freeing all allocated memory, including the object itself, which
// cannot be used again after this. This flushes but does not close the output
// file that was passed to zip_open(), since zip_open() didn't open it. In the
// event that there was a write error in a preceding zip_entry(), nothing is
// written, and the zip object is simply freed. On success, 0 is returned. If
// zip is not valid, then -1 is returned. If there is a write error, then 1 is
// returned.
int zip_close(ZIP *zip);

// Adjust the compression level. See zlib deflateParams() for details
int zip_level(ZIP *zip, int level);

// Error handling notes: All memory allocations are expected to succeed. The
// code aborts immediately with an assert if an allocation or reallocation
// fails. Similarly, the deflate() process and localtime() on the current time
// should not fail, and so assert out if there is an error or unexpected return
// value. If there is a read error on a file after successfully opening it, the
// entry is completed with the partial file data up to the error. However, the
// entry is omitted from the zip file central directory, and a warning message
// is issued. The omission results in the corrupted entry being invisible to
// unzip and other zip file access programs and libraries.
