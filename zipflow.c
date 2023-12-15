/* zipflow.c -- streaming zipper
 * Copyright (C) 2022-2023 Mark Adler
 * For conditions of distribution and use, see copyright notice in zipflow.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <limits.h>
#include <assert.h>
#include "zlib.h"
#include "zipflow.h"

// Maximum two and four-byte field values.
#define MAX16 0xffff
#define MAX32 0xffffffff

// Input and output buffer sizes for deflate.
#if UINT_MAX < 4294967295
#  define CHUNK 32768
#else
#  define CHUNK 262144
#endif

// Information on each entry saved for the central directory. This takes up 64
// to 72 bytes, plus the zero-terminated file name allocation for each entry.
typedef struct {
    char *name;                 // path name (allocated)
    uint16_t nlen;              // path name length
    uint8_t os;                 // operating system (currently 3 or 10)
    uint8_t spare;              // (structure padding)
    uint64_t ulen;              // uncompressed length
    uint64_t clen;              // compressed length
    uint32_t crc;               // CRC-32 of uncompressed data
    uint32_t mode;              // Unix or Windows permissions
    uint64_t ctime;             // Windows creation time
    uint64_t atime;             // Unix or Windows last accessed time
    uint64_t mtime;             // Unix or Windows last modified time
    uint64_t off;               // offset of local header
} head_t;

// zip file state. All path names are built up in the single allocation at
// path, which grows as needed. The list of header information structures at
// head hold the metadata that will be needed for the central directory, and
// grows as needed. strm is a deflate engine that is reused for each entry.
typedef struct {
    void *handle;               // user opaque pointer for put() function
    int (*put)(void *, void const *, size_t, int);   // write streaming data
    FILE *out;                  // output file for streaming data
    unsigned char *data;        // uncompressed deflate input buffer
    unsigned char *comp;        // compressed deflate output buffer
    uint64_t off;               // current offset in zip file
    uint32_t id;                // constant identifier for validity check
    char bad;                   // true if there is a write error
    char omit;                  // true to omit entry in central directory
    char feed;                  // true if feeding data with zip_data()
    char level;                 // requested compression level
    size_t plen;                // path name length
    size_t pmax;                // path name allocation in bytes
    char *path;                 // current path (allocated)
    size_t hnum;                // number of headers
    size_t hmax;                // headers allocation count
    head_t *head;               // list of headers (allocated)
    void *hook;                 // user opaque pointer for log() function
    void (*log)(void *, char *);    // log function
    z_stream strm;              // re-useable deflate engine
} zip_t;

// Constant in zip_t for validity check.
#define ID 3989422804

// Issue a message. If set, use the registered log() function instead of
// writing to stderr.
#define warn(...) \
    zip_msg(zip, __VA_ARGS__)
static void zip_msg(zip_t *zip, char const *fmt, ...) {
    if (zip->log == NULL) {
        fputs("zipflow: ", stderr);
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        putc('\n', stderr);
    }
    else {
        // Get the length of the formatted contents.
        va_list args;
        va_start(args, fmt);
        size_t len = vsnprintf(NULL, 0, fmt, args);
        va_end(args);

        // Construct the message. The "zipflow: " prefix and new line suffix
        // otherwise included on stderr are omitted for the message delivered
        // to the user.
        char *msg = malloc(len + 1);
        assert(msg != NULL && "out of memory");
        va_start(args, fmt);
        vsnprintf(msg, len + 1, fmt, args);
        va_end(args);

        // Pass the allocated meesage to the user log function. It is up to the
        // user to free the allocation when the message is no longer needed.
        zip->log(zip->hook, msg);
    }
}

// Write the size bytes at ptr to the zip file, updating the offset. If ptr is
// NULL, then flush the output. If there is an error, block all subsequent
// writes. All output to the stream goes through this function.
static void zip_put(zip_t *zip, void const *ptr, size_t size, int flag) {
    if (zip->bad)
        return;
    if (zip->put(zip->handle, ptr, size, flag))
        zip->bad = 1;
    else
        zip->off += size;
}

// Default put() function for writing to the file zip->out.
static int zip_write(void *handle, void const *ptr, size_t size, int flag) {
    zip_t *zip = (zip_t *)handle;
    int ret = fwrite(ptr, 1, size, zip->out) < size;
    if (flag == 2 && ret == 0)
        ret = fflush(zip->out);
    if (ret)
        warn("write error: %s -- aborting", strerror(errno));
    return ret;
}

// Allocate, initialize, and return a zip_t structure. Provide starting
// allocations for the path and list of headers. Fire up the deflate engine,
// using level for the compression level.
static ZIP *zip_init(int level) {
    zip_t *zip = malloc(sizeof(zip_t));
    assert(zip != NULL && "out of memory");
    zip->handle = NULL;
    zip->put = NULL;
    zip->out = NULL;
    zip->data = malloc(CHUNK);
    zip->comp = malloc(CHUNK);
    assert(zip->data != NULL && zip->comp != NULL && "out of memory");
    zip->off = 0;
    zip->id = ID;
    zip->bad = 0;
    zip->omit = 0;
    zip->feed = 0;
    zip->level = level;
    zip->plen = 0;
    zip->pmax = 512;
    zip->path = malloc(zip->pmax);
    assert(zip->path != NULL && "out of memory");
    zip->hnum = 0;
    zip->hmax = 512;
    zip->head = malloc(zip->hmax * sizeof(head_t));
    assert(zip->head != NULL && "out of memory");
    zip->hook = NULL;
    zip->log = NULL;
    zip->strm.zalloc = Z_NULL;
    zip->strm.zfree = Z_NULL;
    zip->strm.opaque = Z_NULL;
    int ret = deflateInit2(&zip->strm, level, Z_DEFLATED, -15, 8,
                           Z_DEFAULT_STRATEGY);     // raw deflate
    assert(ret == Z_OK && "out of memory");
    return (ZIP *)zip;
}

// Macros for writing little-endian integers to a byte buffer.
#define PUT2(p, v) \
    do { \
        (p)[0] = (v) & 0xff; \
        (p)[1] = ((v) >> 8) & 0xff; \
    } while (0)
#define PUT4(p, v) \
    do { \
        PUT2(p, v); \
        PUT2((p) + 2, (uint32_t)(v) >> 16); \
    } while (0)
#define PUT8(p, v) \
    do { \
        PUT4(p, v); \
        PUT4((p) + 4, (uint64_t)(v) >> 32); \
    } while (0)

// Convert the Unix time clock to DOS time in the four bytes at *dos. If there
// is a conversion error for any reason, store the current time in DOS format
// at *dos. The Unix time in seconds is rounded up to an even number of
// seconds, since the DOS time can only represent even seconds. If the Unix
// time is before 1980, the minimum DOS time of Jan 1, 1980 is used.
static void put_time(unsigned char *dos, time_t clock) {
    clock += clock & 1;
    struct tm *s = localtime(&clock);
    if (s == NULL) {
        clock = time(NULL);             // on error, use current time
        clock += clock & 1;
        s = localtime(&clock);
        assert(s != NULL && "internal error");
    }
    if (s->tm_year < 80) {              // no DOS time before 1980
        dos[0] = 0;  dos[1] = 0;                // use midnight,
        dos[2] = (1 << 5) + 1;  dos[3] = 0;     // Jan 1, 1980
    }
    else {
        dos[0] = (s->tm_min << 5) + (s->tm_sec >> 1);
        dos[1] = (s->tm_hour << 3) + (s->tm_min >> 3);
        dos[2] = ((s->tm_mon + 1) << 5) + s->tm_mday;
        dos[3] = ((s->tm_year - 80) << 1) + ((s->tm_mon + 1) >> 3);
    }
}

// Representation of compression level for general purpose bit flag.
#define LEVEL() \
    (zip->level >= 9 ? 2 : \
     zip->level == 2 ? 4 : \
     zip->level == 1 ? 6 : 0)

// Write a local header with the information in the last header slot.
static void zip_local(zip_t *zip) {
    head_t const *head = zip->head + zip->hnum;

    // Local header.
    unsigned char local[30];
    PUT4(local, 0x04034b50);        // local file header signature
    PUT2(local + 4,                 // version needed to extract (2.0 or 4.5)
         head->off >= MAX32 ? 45 : 20);
    PUT2(local + 6, 0x808 + LEVEL());   // UTF-8 name, level, data descriptor
    PUT2(local + 8, 8);             // deflate compression method
    put_time(local + 10, head->mtime);  // modified time and date (4 bytes)
    PUT4(local + 14, 0);            // CRC-32 (in data descriptor)
    PUT4(local + 18, 0);            // compressed size (in data descriptor)
    PUT4(local + 22, 0);            // uncompressed size (in data descriptor)
    PUT2(local + 26, head->nlen);   // file name length (name follows header)
    PUT2(local + 28, 0);            // extra field length

    // Write the local header.
    zip_put(zip, local, sizeof(local), 0);
    zip_put(zip, head->name, head->nlen, 1);
}

// Compress the file in using deflate, writing the compressed data to zip->out.
// Set the saved header fields for the uncompressed and compressed lengths, and
// the CRC-32 computed on the uncompressed data. Abandon the deflate process if
// a write error is encountered, which is assumed to be persistent.
static void zip_deflate(zip_t *zip, FILE *in) {
    head_t *head = zip->head + zip->hnum;
    head->ulen = 0;
    head->clen = 0;
    head->crc = crc32(0, Z_NULL, 0);
    zip->strm.avail_in = 0;
    int eof = 0, ret;
    do {
        if (zip->strm.avail_in == 0 && !eof) {
            zip->strm.avail_in = fread(zip->data, 1, CHUNK, in);
            zip->strm.next_in = zip->data;
            head->ulen += zip->strm.avail_in;
            head->crc = crc32(head->crc, zip->data, zip->strm.avail_in);
            if (zip->strm.avail_in < CHUNK) {
                eof = 1;
                if (ferror(in)) {
                    warn("read error on %s: %s -- entry omitted",
                         zip->path, strerror(errno));
                    zip->omit = 1;  // finish, but omit from directory
                }
            }
        }
        zip->strm.avail_out = CHUNK;
        zip->strm.next_out = zip->comp;
        ret = deflate(&zip->strm, eof ? Z_FINISH : Z_NO_FLUSH);
        zip_put(zip, zip->comp, CHUNK - zip->strm.avail_out, 1);
        if (zip->bad)
            return;                 // abandon compression on write error
        head->clen += CHUNK - zip->strm.avail_out;
    } while (ret == Z_OK);
    assert(ret == Z_STREAM_END && "internal error");
    deflateReset(&zip->strm);       // prepare for next use of engine
}

// Write a data descriptor with the information in the last header slot. The
// descriptor can use either 32-bit or 64-bit fields for the compressed and
// uncompressed lengths. The size must be determined by the same logic that
// decides on an extended information field in the central directory header.
// That is why the offset requiring 64-bits will drive this to 64-bits.
static void zip_desc(zip_t *zip) {
    head_t const *head = zip->head + zip->hnum;
    unsigned char desc[24];
    PUT4(desc, 0x08074b50);         // data descriptor signature
    PUT4(desc + 4, head->crc);      // uncompressed data CRC-32
    if (head->ulen >= MAX32 || head->clen >= MAX32 || head->off >= MAX32) {
        // zip64 long compressed and uncompressed lengths
        PUT8(desc + 8, head->clen);
        PUT8(desc + 16, head->ulen);
        zip_put(zip, desc, 24, 1);
    }
    else {
        // legacy short compressed and uncompressed lengths
        PUT4(desc + 8, head->clen);
        PUT4(desc + 12, head->ulen);
        zip_put(zip, desc, 16, 1);
    }
}

// Set up for next zip entry by assuring a slot for the next set of metadata.
static void zip_next(zip_t *zip) {
    if (zip->hnum == zip->hmax) {
        zip->hmax <<= 1;
        zip->head = realloc(zip->head, zip->hmax * sizeof(head_t));
        assert(zip->head != NULL && "out of memory");
    }
}

// Write an entry to the zip file. zip->path is the name of a regular file. The
// operating system and associated file attributes have already been stored at
// zip->head[zip->hnum]. This writes the local header, the compressed data, and
// the data descriptor.
static void zip_file(zip_t *zip) {
    // Check name length.
    if (zip->plen > 65535) {
        warn("file name is too long for the zip format! -- skipping %s",
             zip->path);
        return;
    }

    // Make sure we can open it for reading first. We know it's there, but
    // perhaps we don't have permission to read it.
    FILE *in = fopen(zip->path, "rb");
    if (in == NULL) {
        warn("could not open %s for reading -- skipping", zip->path);
        return;
    }

    // Save the name and local header offset in the header structure.
    head_t *head = zip->head + zip->hnum;

    // Save the name and local header offset in the header structure.
    head->name = malloc(zip->plen + 1);
    assert(head->name != NULL && "out of memory");
    memcpy(head->name, zip->path, zip->plen + 1);
    head->nlen = zip->plen;
    head->off = zip->off;

    // Write the local header, compressed data, and data descriptor, and update
    // the entry count. zip_deflate() sets the CRC-32 and lengths in the header
    // structure. If there is a read error on in, the entry is completed with
    // the data read up to the error, but the entry is omitted from the central
    // directory.
    zip_local(zip);
    zip_deflate(zip, in);
    fclose(in);
    zip_desc(zip);
    if (zip->omit) {
        free(head->name);
        zip->omit = 0;
    }
    else
        zip->hnum++;
}

// Assure that there are at least want bytes available for the path name.
static void zip_room(zip_t *zip, size_t want) {
    size_t need = zip->pmax;
    while (need < want)
        need <<= 1;
    if (need == zip->pmax)
        return;
    zip->path = realloc(zip->path, need);
    assert(zip->path != NULL && "out of memory");
    zip->pmax = need;
}

// Look for regular files to put in the zip file, recursively descending into
// the directories. If zip->path is a regular file, then zip it. If zip->path
// is a directory, call zip_scan() with each of the entries in that directory.
// If zip->path is neither, issue a warning and go on to the next name.
// Symbolic links are treated as the objects that they link to. zip_scan() is
// operating system dependent.
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <locale.h>
#  define OS 10
static void zip_scan(zip_t *zip) {
    // Get the metadata for the object named zip->path. We need to open the
    // object in case it's a symbolic link.
    setlocale(LC_ALL, ".utf8");
    HANDLE obj = CreateFileA(zip->path, GENERIC_READ, FILE_SHARE_READ, NULL,
                             OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (obj == INVALID_HANDLE_VALUE) {
        warn("could not open %s (%u) -- skipping", zip->path, GetLastError());
        return;
    }
    BY_HANDLE_FILE_INFORMATION info;
    int ret = GetFileInformationByHandle(obj, &info);
    CloseHandle(obj);
    if (ret == 0) {
        warn("get %s info failed (%u) -- skipping", zip->path, GetLastError());
        return;
    }

    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        // zip->path is a directory. Open and traverse the directory.
        size_t len = zip->plen;
        zip_room(zip, len + 3);
        memcpy(zip->path + len, "\\*", 3);
        WIN32_FIND_DATAA meta;
        HANDLE dir = FindFirstFileA(zip->path, &meta);
        if (dir == INVALID_HANDLE_VALUE) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) {
                warn("could not open directory %s (%u) -- skipping",
                     zip->path, GetLastError());
                return;
            }
        }
        else {
            do {
                char const *name = meta.cFileName;
                if (name[0] == '.' && (name[1] == 0 ||
                                       (name[1] == '.' && name[2] == 0)))
                    continue;           // ignore . and .. directories
                size_t nlen = strlen(name);
                if (memchr(name, '?', nlen) != NULL) {
                    // The name could not be represented -- use alternate name.
                    name = meta.cAlternateFileName;
                    nlen = strlen(name);
                }
                // Append the name to zip->path. Recursively process the new
                // zip->path.
                zip_room(zip, len + 1 + nlen + 1);
                memcpy(zip->path + len + 1, name, nlen + 1);
                zip->plen = len + 1 + nlen;
                zip_scan(zip);
            } while (FindNextFileA(dir, &meta));
            FindClose(dir);
        }

        // Restore zip->path to what it was.
        zip->path[len] = 0;
        zip->plen = len;
        return;
    }

    if ((info.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        // zip->path is a device or a symbolic link to a directory. We discard
        // the latter in order to avoid recursion loops.
        warn("%s is not a file or directory -- skipping", zip->path);
        return;
    }

    // zip->path is a regular file, or a symbolic link to one. zip it,
    // providing the associated file metadata to include in the zip file.
    // Assure that there is room in the header list to add an entry.
    zip_next(zip);
    head_t *head = zip->head + zip->hnum;
    head->os = OS;
    head->mode = info.dwFileAttributes;
    head->ctime = info.ftCreationTime.dwLowDateTime |
                  ((uint64_t)info.ftCreationTime.dwHighDateTime << 32);
    head->atime = info.ftLastAccessTime.dwLowDateTime |
                  ((uint64_t)info.ftLastAccessTime.dwHighDateTime << 32);
    head->mtime = info.ftLastWriteTime.dwLowDateTime |
                  ((uint64_t)info.ftLastWriteTime.dwHighDateTime << 32);
    zip_file(zip);
}
#else   // Unix (assumes POSIX compatible)
#  include <sys/stat.h>
#  include <dirent.h>
#  define OS 3
static void zip_scan(zip_t *zip) {
    // Get the metadata for the object named zip->path.
    struct stat st;
    int ret = stat(zip->path, &st);
    if (ret) {
        warn("could not stat %s -- skipping", zip->path);
        return;
    }

    if ((st.st_mode & S_IFMT) == S_IFDIR) {
        // zip->path is a directory. Open and traverse the directory.
        DIR *dir = opendir(zip->path);
        if (dir == NULL) {
            warn("could not open directory %s -- skipping", zip->path);
            return;
        }
        size_t len = zip->plen;
        zip->path[len] = '/';
        struct dirent *dp;
        while ((dp = readdir(dir)) != NULL) {
            char const *name = dp->d_name;
            if (name[0] == '.' && (name[1] == 0 ||
                                   (name[1] == '.' && name[2] == 0)))
                continue;           // ignore . and .. directories
            // Append a slash and the name to zip->path. Recursively process
            // the new zip->path.
            size_t nlen = strlen(name);
            zip_room(zip, len + 1 + nlen + 1);
            memcpy(zip->path + len + 1, name, nlen + 1);
            zip->plen = len + 1 + nlen;
            zip_scan(zip);
        }
        closedir(dir);

        // Restore zip->path to what it was.
        zip->path[len] = 0;
        zip->plen = len;
        return;
    }

    if ((st.st_mode & S_IFMT) != S_IFREG) {
        // zip->path may be a device, pipe, or socket.
        warn("%s is not a file or directory -- skipping", zip->path);
        return;
    }

    // zip->path is a regular file, or a symbolic link to one. zip it,
    // providing the associated file metadata to include in the zip file.
    // Assure that there is room in the header list to add an entry.
    zip_next(zip);
    head_t *head = zip->head + zip->hnum;
    head->os = OS;
    head->mode = (uint32_t)st.st_mode << 16;
    head->atime = st.st_atime;
    head->mtime = st.st_mtime;
    zip_file(zip);
}
#endif

// Write a central directory entry with the information in head.
static void zip_central(zip_t *zip, head_t const *head) {
    // Zip64 extended information field. If zlen ends up zero, then not needed.
    unsigned char zip64[28];
    unsigned zlen = 0;
    if (head->ulen >= MAX32) {      // oddly ulen then clen, opposite headers
        PUT8(zip64 + 4 + zlen, head->ulen);
        zlen += 8;
    }
    if (head->clen >= MAX32) {
        PUT8(zip64 + 4 + zlen, head->clen);
        zlen += 8;
    }
    if (head->off >= MAX32) {
        PUT8(zip64 + 4 + zlen, head->off);
        zlen += 8;
    }
    if (zlen) {
        PUT2(zip64, 1);             // zip64 extended information id
        PUT2(zip64 + 2, zlen);
        zlen += 4;
    }

    // Generate an extra field for UTC timestamps.
    unsigned char stamp[36];
    size_t xlen = 0;
    if (head->os == 3) {
        // Unix timestamps extra field.
        PUT2(stamp, 13);                // PKWare id for Unix timestamps
        PUT2(stamp + 2, 8);             // length of the remainder
        PUT4(stamp + 4, head->atime);   // last accessed time
        PUT4(stamp + 8, head->mtime);   // last modified time
        xlen = 12;
    }
    else {      // head->os == 10
        // Windows timestamps extra field.
        PUT2(stamp, 10);                // NTFS extra field
        PUT2(stamp + 2, 32);            // length of the remainder
        PUT4(stamp + 4, 0);             // (reserved by PKWare)
        PUT2(stamp + 8, 1);             // tag for timestamps
        PUT2(stamp + 10, 24);           // length of the tag data
        PUT8(stamp + 12, head->mtime);  // last write time
        PUT8(stamp + 20, head->atime);  // last access time
        PUT8(stamp + 28, head->ctime);  // creation time
        xlen = 36;
    }

    // Central directory header. Any offset or lengths that don't fit here are
    // replaced with the max value for the field, and appear instead in the
    // extended information field.
    unsigned char central[46];
    PUT4(central, 0x02014b50);      // central directory header signature
    PUT2(central + 4,               // os, made by v4.5 equivalent
         ((unsigned)head->os << 8) + 45);
    PUT2(central + 6, zlen ? 45 : 20);  // version needed to extract
    PUT2(central + 8, 0x808 + LEVEL()); // UTF-8 name, level, data descriptor
    PUT2(central + 10, 8);          // deflate compression method
    put_time(central + 12, head->mtime);    // modified time and date (4 bytes)
    PUT4(central + 16, head->crc);  // CRC-32
    PUT4(central + 20,              // compressed length
         head->clen >= MAX32 ? MAX32 : head->clen);
    PUT4(central + 24,              // uncompressed length
         head->ulen >= MAX32 ? MAX32 : head->ulen);
    PUT2(central + 28, head->nlen); // file name length (name after header)
    PUT2(central + 30, zlen + xlen);    // extra field length (after name)
    PUT2(central + 32, 0);          // file comment length
    PUT2(central + 34, 0);          // starting disk
    PUT2(central + 36, 0);          // internal file attributes
    PUT4(central + 38, head->mode); // Unix file attributes
    PUT4(central + 42,              // local header offset
         head->off >= MAX32 ? MAX32 : head->off);

    // Write central directory header.
    zip_put(zip, central, sizeof(central), 0);
    zip_put(zip, head->name, head->nlen, 0);
    zip_put(zip, zip64, zlen, 0);
    zip_put(zip, stamp, xlen, 1);
}

// Write the zip file end records. The central directory started at offset beg
// and ended at the current offset.
static void zip_end(zip_t *zip, uint64_t beg) {
    // If the count, length, or offset doesn't fit in the end of central
    // directory record, then write the zip64 record and locator to hold and
    // find them.
    uint64_t len = zip->off - beg;
    if (zip->hnum >= MAX16 || len >= MAX32 || beg >= MAX32) {
        // zip64 end of central directory record.
        unsigned char xend[56];
        PUT4(xend, 0x06064b50);     // zip64 end record signature
        PUT8(xend + 4, 44);         // length of remaining record
        PUT2(xend + 12, 45);        // version made by (4.5)
        PUT2(xend + 14, 45);        // version needed to extract (4.5)
        PUT4(xend + 16, 0);         // number of this disk
        PUT4(xend + 20, 0);         // number of disk with central directory
        PUT8(xend + 24, zip->hnum); // number of directory entries here
        PUT8(xend + 32, zip->hnum); // total number of directory entries
        PUT8(xend + 40, len);       // length of central directory
        PUT8(xend + 48, beg);       // offset of central directory

        // zip64 end of central directory locator.
        unsigned char loc[20];
        PUT4(loc, 0x07064b50);      // zip64 end locator signature
        PUT4(loc + 4, 0);           // number of disk with zip64 end record
        PUT8(loc + 8, zip->off);    // offset of zip64 end record
        PUT4(loc + 16, 1);          // total number of disks

        // Write the zip64 records.
        zip_put(zip, xend, sizeof(xend), 0);
        zip_put(zip, loc, sizeof(loc), 1);
    }

    // end of central directory record.
    unsigned char end[22];
    PUT4(end, 0x06054b50);          // end record signature
    PUT2(end + 4, 0);               // number of this disk
    PUT2(end + 6, 0);               // start of central directory disk
    PUT2(end + 8,                   // number of directory entries on this disk
         zip->hnum >= MAX16 ? MAX16 : zip->hnum);
    PUT2(end + 10,                  // total number of directory entries
         zip->hnum >= MAX16 ? MAX16 : zip->hnum);
    PUT4(end + 12,                  // central directory length
         len >= MAX32 ? MAX32 : len);
    PUT4(end + 16,                  // central directory start offset
         beg >= MAX32 ? MAX32 : beg);
    PUT2(end + 20, 0);              // zip file comment length (after record)

    // Write the end record. This completes the zip file.
    zip_put(zip, end, sizeof(end), 2);
}

// Free all allocated memory. Return true if a write error was noted.
static int zip_clean(zip_t *zip) {
    deflateEnd(&zip->strm);
    while (zip->hnum)
        free(zip->head[--zip->hnum].name);
    free(zip->head);
    free(zip->path);
    free(zip->comp);
    free(zip->data);
    int bad = zip->bad;
    zip->id = 0;
    free(zip);
    return bad;
}

// ------ exposed functions ------

// See comments in zipflow.h.
ZIP *zip_open(FILE *out, int level) {
    if (out == NULL || level < -1 || level > Z_BEST_COMPRESSION)
        return NULL;
    zip_t *zip = zip_init(level);
    zip->out = out;
    zip->handle = zip;
    zip->put = zip_write;
    return (ZIP *)zip;
}

// See comments in zipflow.h.
ZIP *zip_pipe(void *handle, int (*put)(void *, void const *, size_t, int),
              int level) {
    if (put == NULL || level < -1 || level > Z_BEST_COMPRESSION)
        return NULL;
    zip_t *zip = zip_init(level);
    zip->handle = handle;
    zip->put = put;
    return (ZIP *)zip;
}

// See comments in zipflow.h.
int zip_log(ZIP *ptr, void *hook, void (*log)(void *, char *)) {
    zip_t *zip = (zip_t *)ptr;
    if (zip == NULL || zip->id != ID)
        return -1;
    zip->hook = hook;
    zip->log = log;
    return 0;
}

// See comments in zipflow.h.
int zip_entry(ZIP *ptr, char const *path) {
    zip_t *zip = (zip_t *)ptr;
    if (zip == NULL || zip->id != ID || path == NULL || zip->feed)
        return -1;
    size_t len = strlen(path);
    zip_room(zip, len + 1);
    memcpy(zip->path, path, len + 1);
    zip->plen = len;
    zip_scan(zip);
    return zip->bad;
}

// See comments in zipflow.h.
int zip_meta(ZIP *ptr, char const *path, int os, ...) {
    zip_t *zip = (zip_t *)ptr;
    if (zip == NULL || zip->id != ID || path == NULL || zip->feed)
        return -1;
    size_t len = strlen(path);
    if (len > 65535)
        return -1;                  // path name too long for zip format

    // For now, only allow os == 3 for Unix or 10 for Windows.
    if (os != 3 && os != 10)
        return -1;

    // Save the path name for the header.
    zip_next(zip);
    head_t *head = zip->head + zip->hnum;
    head->name = malloc(len + 1);
    assert(head->name != NULL && "out of memory");
    memcpy(head->name, path, len + 1);
    head->nlen = len;

    // Save provided OS-specific (Unix) header information.
    head->os = os;
    va_list args;
    va_start(args, os);
    if (os == 3) {
        head->mode =
            (uint32_t)(0100000 | (va_arg(args, unsigned) & 07777)) << 16;
        head->atime = va_arg(args, uint32_t);
        head->mtime = va_arg(args, uint32_t);
    }
    else {      // os == 10
        head->mode = va_arg(args, uint32_t);
        head->ctime = va_arg(args, uint64_t);
        head->atime = va_arg(args, uint64_t);
        head->mtime = va_arg(args, uint64_t);
    }
    va_end(args);

    // Set up for writing the entry with zip_data().
    head->off = zip->off;
    head->ulen = 0;
    head->clen = 0;
    head->crc = crc32(0, Z_NULL, 0);
    zip->feed = 1;
    return 0;
}

// See comments in zipflow.h.
int zip_data(ZIP *ptr, void const *data, size_t len, int last) {
    zip_t *zip = (zip_t *)ptr;
    if (zip == NULL || zip->id != ID || zip->feed == 0 ||
        (data == NULL && len != 0))
        return -1;
    if (len == 0 && last == 0)
        // Nothing to do.
        return zip->bad;

    if (zip->feed == 1) {
        // Write local header once before any compressed data.
        zip_local(zip);
        zip->feed = 2;
    }

    // Update the CRC-32 and uncompressed length.
    head_t *head = zip->head + zip->hnum;
    if (len) {
        head->crc = crc32_z(head->crc, data, len);
        head->ulen += len;
    }

    // Compress the data to the output stream, updating the compressed length.
    zip->strm.avail_in = 0;
    zip->strm.next_in = (unsigned char *)(uintptr_t)data;   // awful hack
    int ret;
    do {
        unsigned more = UINT_MAX - zip->strm.avail_in;
        if (more > len)
            more = (unsigned)len;
        zip->strm.avail_in += more;
        len -= more;
        zip->strm.avail_out = CHUNK;
        zip->strm.next_out = zip->comp;
        ret = deflate(&zip->strm, last && len == 0 ? Z_FINISH : Z_NO_FLUSH);
        zip_put(zip, zip->comp, CHUNK - zip->strm.avail_out, 1);
        if (zip->bad)
            return zip->bad;            // abandon compression on write error
        head->clen += CHUNK - zip->strm.avail_out;
        // Continue until all input consumed and all output delivered. If last
        // is false, this loop will exit after a final unproductive call of
        // deflate(), which returns Z_BUF_ERROR.
    } while (ret == Z_OK);

    if (last) {
        // Complete the zip file entry and terminate feed mode.
        assert(ret == Z_STREAM_END && "internal error");
        deflateReset(&zip->strm);       // prepare for next use of engine
        zip_desc(zip);
        zip->hnum++;
        zip->feed = 0;
    }
    else
        assert(ret == Z_BUF_ERROR && "internal error");
    return zip->bad;
}

// See comments in zipflow.h.
int zip_close(ZIP *ptr) {
    zip_t *zip = (zip_t *)ptr;
    if (zip == NULL || zip->id != ID)
        return -1;
    if (zip->feed && !zip->bad)
        // Assure zip_close() can always be used, and does something sensible.
        zip_data(zip, NULL, 0, 1);

    // Write the trailing metadata and flush the output stream.
    uint64_t beg = zip->off;
    for (size_t i = 0; i < zip->hnum && !zip->bad; i++)
        zip_central(zip, zip->head + i);
    zip_end(zip, beg);
    return zip_clean(zip);
}
