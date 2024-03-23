#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include "buffer.h"

struct buffer {
	size_t cap;
	size_t rh, wh;
	char data[];
};

Buffer *bf_new(size_t cap) {
	Buffer *bf = malloc(sizeof *bf + cap);
	if (bf == NULL) {
		perror("malloc");
		return NULL;
	}
	*bf = (Buffer) { .cap = cap, .rh = 0, .wh = 0, };
	return bf;
}

void bf_free(Buffer *bf) {
	free(bf);
}

void bf_reset(Buffer *bf) {
	bf->rh = 0;
	bf->wh = 0;
}

size_t bf_cap(Buffer *bf) { return bf->cap; }
size_t bf_size(Buffer *bf) { return bf->wh - bf->rh; }

static void _bf_pack(Buffer *bf) {
	if (bf->rh == 0)
		return;
	size_t size = bf_size(bf);
	memmove(bf->data, &bf->data[bf->rh], size);
	bf->rh = 0;
	bf->wh = size;
}

static size_t _bf_space(Buffer *bf) {
	_bf_pack(bf);
	return bf->cap - bf->wh;
}

int bf_read(Buffer *bf, int fd) {
	size_t rd_size = _bf_space(bf);
	if (rd_size > BF_IOSZ)
		rd_size = BF_IOSZ;
	if (rd_size == 0)
		return 0;

	ssize_t n = read(fd, &bf->data[bf->wh], rd_size);
	if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
		perror("read");
	if (n <= 0)
		return n;
	bf->wh += n;
	return 1;
}

int bf_readall(Buffer *bf, int fd) {
	int ok;
	while ((ok = bf_read(bf, fd)) > 0);
	if (ok == -1)
		return -1;
	return 1;
}

int bf_readuntil(Buffer *bf, int fd, void *delim, size_t delim_size) {
	assert(delim_size > 0);

	// already in buffer
	if (memmem(&bf->data[bf->rh], bf_size(bf), delim, delim_size) != NULL)
		return 1;

	int ok;
	while ((ok = bf_read(bf, fd)) > 0)
		if (memmem(&bf->data[bf->rh], bf_size(bf), delim, delim_size) != NULL)
			return 1;
	if (ok == 0)
		fprintf(stderr, "unexpected EOF\n");
	return -1;
}

int bf_readn(Buffer *bf, int fd, size_t size) {
	assert(size <= bf->cap);

	// already in buffer
	if (bf_size(bf) >= size)
		return 1;

	int ok;
	while ((ok = bf_read(bf, fd)) > 0)
		if (bf_size(bf) >= size)
			return 1;
	if (ok == 0)
		fprintf(stderr, "unexpected EOF\n");
	return -1;
}

int bf_write(Buffer *bf, int fd) {
	size_t wr_size = bf_size(bf);
	if (wr_size > BF_IOSZ)
		wr_size = BF_IOSZ;
	if (wr_size == 0)
		return 0;

	ssize_t n = write(fd, &bf->data[bf->rh], wr_size);
	if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)
		perror("read");
	if (n < 0)
		return n;
	bf->rh += n;
	return 1;
}

int bf_writeall(Buffer *bf, int fd) {
	int ok;
	while ((ok = bf_write(bf, fd)) > 0);
	if (ok == -1)
		return -1;
	return bf_size(bf) == 0;
}

int bf_writen(Buffer *bf, int fd, size_t size) {
	assert(size <= bf_size(bf));

	size_t off = bf_size(bf) - size;
	bf->wh -= off;

	int ok;
	while ((ok = bf_write(bf, fd)) > 0) {
		if (bf_size(bf) == 0) {
			bf->wh += off;
			return 1;
		}
	}
	bf->wh += off;
	if (ok == -1)
		return -1;
	return 0;
}

char *bf_data_delim(Buffer *bf, size_t *ret_size, void *delim, size_t delim_size) {
	char *found = memmem(&bf->data[bf->rh], bf->wh - bf->rh, delim, delim_size);
	if (found == NULL)
		return NULL;

	char *start = &bf->data[bf->rh];
	*ret_size = found - start + delim_size;
	bf->rh += *ret_size;
	return start;
}

int bf_write_data(Buffer *bf, void *data, size_t size) {
	if (_bf_space(bf) < size)
		return -1;
	memcpy(&bf->data[bf->wh], data, size);
	bf->wh += size;
	return 1;
}

// for populating buffers with eg. sprintf
char *bf_data_raw(Buffer *bf) {
	return bf->data;
}
void bf_seth_raw(Buffer *bf, size_t rh, size_t wh) {
	bf->rh = rh;
	bf->wh = wh;
}
