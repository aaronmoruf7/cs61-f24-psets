ssize_t io61_write(io61_file* f, const unsigned char* buf, size_t sz) {
    size_t nwritten = 0;
    ssize_t nw = 0;

    while (nwritten != sz) {
        //if cache is full flush it
        if (f -> cache_size == BLOCK_SIZE){
            nw = write(f->fd, f -> cache, f -> cache_size);
            if (nw < 0) {
                return -1;
            }
            f -> cache_size -= nw;
            memmove(f->cache, f->cache + nw, f->cache_size); 

        }
        
        // if cache not full, copy from buf to cache
        size_t to_copy = min(BLOCK_SIZE - f -> cache_size, sz - nwritten);

        memcpy(&f -> cache[f -> cache_size], buf + nwritten, to_copy);

        f -> cache_size += to_copy;
        nwritten += to_copy;

    }

    return nwritten;
 
}