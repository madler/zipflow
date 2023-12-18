/* zips.c -- streaming zipper
 * Copyright (C) 2022 Mark Adler
 * For conditions of distribution and use, see copyright notice in zipflow.h
 */

// Write a zip file to stdout containing the files named on the command line,
// and any files contained at any level in the directories named on the command
// line. Symbolic links are treated as the objects they link to. Non-regular
// files (devices, pipes, sockets, etc.) are skipped.

#include <stdio.h>
#include "zipflow.h"

// Change the mode of an open file, like stdout, to binary in Windows.
#if defined(_WIN32) || defined(__CYGWIN__)
#  include <fcntl.h>
#  include <io.h>
#  define SET_BINARY_MODE(file) setmode(fileno(file), O_BINARY)
#else
#  define SET_BINARY_MODE(file)
#endif

int main(int argc, char **argv) {
    SET_BINARY_MODE(stdout);
    ZIP *zip = zip_open(stdout, -1);
    for (int i = 1; i < argc; i++) {
	if (zip_level(zip, -1))
            break;
        if (zip_entry(zip, argv[i]))
            break;
    }
    return zip_close(zip);
}
