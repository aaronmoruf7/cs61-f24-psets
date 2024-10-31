#include "io61.hh"
#include <sys/types.h>
#include <sys/stat.h>
#include <climits>
#include <cerrno>

// io61.cc
//    YOUR CODE HERE!
#define BLOCK_SIZE 4096

// io61_file
//    Data structure for io61 file wrappers. Add your own stuff.

struct io61_file {
    int fd = -1;     // file descriptor
    int mode;        // open mode (O_RDONLY or O_WRONLY)
    unsigned char cache [BLOCK_SIZE];    //cache 
    int cache_size = 0;
    int cache_offset = 0;
    int cache_start = 0;

};


// io61_fdopen(fd, mode)
//    Returns a new io61_file for file descriptor `fd`. `mode` is either
//    O_RDONLY for a read-only file or O_WRONLY for a write-only file.
//    You need not support read/write files.

io61_file* io61_fdopen(int fd, int mode) {
    assert(fd >= 0);
    io61_file* f = new io61_file;
    f->fd = fd;
    f->mode = mode;
    return f;
}


// io61_close(f)
//    Closes the io61_file `f` and releases all its resources.

int io61_close(io61_file* f) {
    io61_flush(f);
    int r = close(f->fd);
    delete f;
    return r;
}


// io61_readc(f)
//    Reads a single (unsigned) byte from `f` and returns it. Returns EOF,
//    which equals -1, on end of file or error.

int io61_readc(io61_file* f) {
    unsigned char ch;
    ssize_t nr = read(f->fd, &ch, 1);
    if (nr == 1) {
        return ch;
    } else if (nr == 0) {
        errno = 0; // clear `errno` to indicate EOF
        return -1;
    } else {
        assert(nr == -1 && errno > 0);
        return -1;
    }
}

size_t min(size_t a, size_t b) {
    return a < b ? a : b;
}

// BLOCK_SIZE < to_read ? BLOCK_SIZE : to_read min()


// io61_read(f, buf, sz)
//    Reads up to `sz` bytes from `f` into `buf`. Returns the number of
//    bytes read on success. Returns 0 if end-of-file is encountered before
//    any bytes are read, and -1 if an error is encountered before any
//    bytes are read.
//
//    Note that the return value might be positive, but less than `sz`,
//    if end-of-file or error is encountered before all `sz` bytes are read.
//    This is called a “short read.”

// ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
//     size_t nread = 0;
//     while (nread != sz) {
//         size_t to_read = sz - nread;
//         ssize_t nr = read(f->fd, buf + nread, min(BLOCK_SIZE, to_read));
        
//         if (nr > 0){
//             nread += nr;
//         }
//         else if (nr == 0) {
//             break;
//         } else {
//             assert(nr == -1 && errno > 0);
//             return -1;
//         }
//     }

//     return nread;

// }

ssize_t io61_read(io61_file* f, unsigned char* buf, size_t sz) {
    size_t nread = 0;
    ssize_t nr = 0;

    while (nread != sz) {
        //check the cache to see if it is empty, if so then refill it
        if (f -> cache_offset == f -> cache_size){
            nr = read(f->fd, f -> cache, BLOCK_SIZE);
            if (nr == 0) {
                break;
            } else if (nr < 0) {
                return -1;
            }
            f -> cache_size = nr;
            f -> cache_offset = 0;
        }
        
        // copy from cache to buf
        size_t to_copy = min(f -> cache_size - f -> cache_offset, sz - nread);

        memcpy(buf + nread, &f -> cache[f -> cache_offset], to_copy);

        f -> cache_offset += to_copy;
        nread += to_copy;

    }

    return nread;
 
}



// io61_writec(f)
//    Write a single character `c` to `f` (converted to unsigned char).
//    Returns 0 on success and -1 on error.

int io61_writec(io61_file* f, int c) {
    unsigned char ch = c;
    ssize_t nw = write(f->fd, &ch, 1);
    if (nw == 1) {
        return 0;
    } else {
        return -1;
    }
}


// io61_write(f, buf, sz)
//    Writes `sz` characters from `buf` to `f`. Returns `sz` on success.
//    Can write fewer than `sz` characters when there is an error, such as
//    a drive running out of space. In this case io61_write returns the
//    number of characters written, or -1 if no characters were written
//    before the error occurred.

ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    size_t nwritten = 0;
    while (nwritten != sz) {
        size_t to_write = sz - nwritten;
        ssize_t nw = write(f->fd, buf + nwritten, BLOCK_SIZE < to_write ? BLOCK_SIZE : to_write);
        if (nw > 0){
            nwritten += nw;
        }
        else if (nw == 0) {
            break;
        } else {
            assert(nw == -1 && errno > 0);
            return -1;
        }
    }
    
    return nwritten;
}


// io61_flush(f)
//    If `f` was opened write-only, `io61_flush(f)` forces a write of any
//    cached data written to `f`. Returns 0 on success; returns -1 if an error
//    is encountered before all cached data was written.
//
//    If `f` was opened read-only, `io61_flush(f)` returns 0. It may also
//    drop any data cached for reading.

int io61_flush(io61_file* f) {
    (void) f;
    return 0;
}


// io61_seek(f, off)
//    Changes the file pointer for file `f` to `off` bytes into the file.
//    Returns 0 on success and -1 on failure.

int io61_seek(io61_file* f, off_t off) {
    off_t r = lseek(f->fd, (off_t) off, SEEK_SET);
    // Ignore the returned offset unless it’s an error.
    if (r == -1) {
        return -1;
    } else {
        return 0;
    }
}


// You shouldn't need to change these functions.

// io61_open_check(filename, mode)
//    Opens the file corresponding to `filename` and returns its io61_file.
//    If `!filename`, returns either the standard input or the
//    standard output, depending on `mode`. Exits with an error message if
//    `filename != nullptr` and the named file cannot be opened.

io61_file* io61_open_check(const char* filename, int mode) {
    int fd;
    if (filename) {
        fd = open(filename, mode, 0666);
    } else if ((mode & O_ACCMODE) == O_RDONLY) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        exit(1);
    }
    return io61_fdopen(fd, mode & O_ACCMODE);
}


// io61_fileno(f)
//    Returns the file descriptor associated with `f`.

int io61_fileno(io61_file* f) {
    return f->fd;
}


// io61_filesize(f)
//    Returns the size of `f` in bytes. Returns -1 if `f` does not have a
//    well-defined size (for instance, if it is a pipe).

off_t io61_filesize(io61_file* f) {
    struct stat s;
    int r = fstat(f->fd, &s);
    if (r >= 0 && S_ISREG(s.st_mode)) {
        return s.st_size;
    } else {
        return -1;
    }
}
