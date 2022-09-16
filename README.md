Synopsis
--------

_zipflow_ is a library of routines to generate and stream zip files. See
[zipflow.h](https://github.com/madler/zipflow/blob/main/zipflow.h) for all the
details.

Motivation
----------

Normally, zip file processing requires random access to the zip file on mass
storage or in memory. _zipflow_ does not require such random access, and does
not require that the entire zip file be in mass storage or memory at any time.
No matter how large the input files are or how large the resulting zip file is,
the amount of memory used by this code for the input and output data, as well
as for the compression process, is small and constant, less than 800 KiB.
Additional memory is used to save metadata on the files written to the zip
file, proportional to the number of files. This is required to be able to write
the zip directory at the end of the zip file. That amount of memory is equal to
a small constant plus the average length of a file name, multiplied by the
number of entries in the resulting zip file. The small constant is 64 to 72
bytes plus the null termination and allocation overhead for the file name.

Usage
------------

Compile your code with zipflow.c and -lz (zlib). Example programs are provided,
zips and fzip, which can be compiled thusly:

    cc -o zips zips.c zipflow.c -lz
    cc -o fzip fzip.c zipflow.c -lz

Test
----

    ./zips *.c > test.zip

will compress the source files into test.zip.

License
-------

This code is under the zlib license, found in the source file and LICENSE.
