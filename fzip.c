/* fzip.c -- zip filter
 * Copyright (C) 2022 Mark Adler
 * For conditions of distribution and use, see copyright notice in zipflow.h
 */

// Simple example of a zip filter, which streams out a zip file on stdout with
// a single entry containing the data read from stdin. The file name used for
// the entry in the zip file is provided on the command line.

#include <stdio.h>
#include <time.h>
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
    if (argc != 2) {
        fputs("usage:\n"
              "    fzip name < infile > outfile\n"
              "    inprog | fzip name | outprog\n"
              "'name' is the zip file entry name\n", stderr);
        return 1;
    }
    SET_BINARY_MODE(stdin);
    SET_BINARY_MODE(stdout);
    ZIP *zip = zip_open(stdout, -1);
    time_t now = time(NULL);
    zip_meta(zip, argv[1], 3, 0644, now, now);
    unsigned char in[32768];
    size_t got;
    while ((got = fread(in, 1, sizeof(in), stdin)) == sizeof(in))
        if (zip_data(zip, in, got, 0))
            return zip_close(zip);
    zip_data(zip, in, got, 1);
    return zip_close(zip);
}
