#include <sys/types.h>

#define BF_IOSZ 4096

typedef struct buffer Buffer;

Buffer *bf_new(size_t cap);
void bf_free(Buffer *bf);
void bf_reset(Buffer *bf);

size_t bf_cap(Buffer *bf);
size_t bf_size(Buffer *bf);

// -1 on error, 0 on EOF or full buffer, 1 on success
int bf_read(Buffer *bf, int fd);
// read until EOF or full buffer
int bf_readall(Buffer *bf, int fd);
// read until delim is in the read buffer
int bf_readuntil(Buffer *bf, int fd, void *delim, size_t delim_size);
// read until size bytes are available
int bf_readn(Buffer *bf, int fd, size_t size);

// -1 on error, 1 on success
int bf_write(Buffer *bf, int fd);
int bf_writeall(Buffer *bf, int fd);
int bf_writen(Buffer *bf, int fd, size_t size);

char *bf_data(Buffer *bf, size_t *ret_size);
char *bf_data_delim(Buffer *bf, size_t *ret_size, void *delim, size_t delim_size);
int bf_write_data(Buffer *bf, void *data, size_t size);

char *bf_data_raw(Buffer *bf);
size_t bf_cap_raw(Buffer *bf);
void bf_seth_raw(Buffer *bf, size_t rh, size_t wh);
