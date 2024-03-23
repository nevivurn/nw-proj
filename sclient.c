#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>


// buffer.h
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

// buffer.c
#define _GNU_SOURCE

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>


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

// client.c

static int dial_server(const char *host, const char *port);
static int send_header(int sfd, const char *host, size_t size);

// RFC 1945 defines LWS as [CRLF] 1*( SP | HT )
// but our assignment spec specifies isspace().
// Thus, we include all whitespace recognized by isspace() in POSIX locale.
// (include \f and \v)
#define WHITESPACE " \f\n\r\t\v"

#define MAX_BODY_SIZE (10 * 1024 * 1024)

static void usage(char *argv[]) {
	fprintf(stderr, "usage: %s -s <host> -p <port>\n", argv[0]);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	// parse args
	int opt;
	char *host = NULL, *port = NULL;
	while ((opt = getopt(argc, argv, "s:p:")) != -1) {
		switch (opt) {
			case 's':
				host = optarg;
				break;
			case 'p':
				port = optarg;
				char *endptr;
				int nport = strtol(port, &endptr, 10);
				if (endptr == port || *endptr != '\0')
					usage(argv);
				if (nport < 1024 || nport > 65535) {
					printf("port number should be between 1024 ~ 65535.\n");
					exit(EXIT_FAILURE);
				}
				break;
			default:
				usage(argv);
		}
	}
	if (host == NULL || port == NULL)
		usage(argv);

	int status = EXIT_FAILURE;
	Buffer *bf = NULL;
	int sfd = -1;

	bf = bf_new(MAX_BODY_SIZE);
	if (bf == NULL)
		goto cleanup;

	// read input
	if (bf_readall(bf, STDIN_FILENO) == -1)
		goto cleanup;
	size_t input_size = bf_size(bf);
	if (input_size == 0) {
		fprintf(stderr, "input must be >0 bytes\n");
		goto cleanup;
	}

	// connect to server
	sfd = dial_server(host, port);
	if (sfd == -1)
		goto cleanup;

	// send request
	if (send_header(sfd, host, input_size) == -1)
		goto cleanup;

	if (bf_writeall(bf, sfd) == -1)
		goto cleanup;

	// header parsing
	int status_ok = 0;
	ssize_t resp_size = -1;

	while (1) {
		if (bf_readuntil(bf, sfd, "\r\n", 2) == -1) {
			fprintf(stderr, "failed reading headers\n");
			goto cleanup;
		}

		size_t data_size;
		char *data = bf_data_delim(bf, &data_size, "\r\n", 2);

		if (data_size >= 1024) {
			fprintf(stderr, "header too long\n");
			goto cleanup;
		}

		if (data_size == 2) // empty line, end of headers
			break;

		// for convenience, headers are \0-terminated strings
		// just make sure that it doesn't contain \0, as disallowed by the spec
		if (memchr(data, '\0', data_size) != NULL) {
			fprintf(stderr, "invalid NULL in header\n");
			goto cleanup;
		}
		data[data_size-2] = '\0'; // replace the \r
		data_size -= 1;

		// no header line can start with whitespace
		if (strchr(WHITESPACE, data[0]) != NULL) {
			fprintf(stderr, "failed parsing headers\n");
			goto cleanup;
		}

		if (!status_ok) { // first header
			// Save the status line, may need to dump it later
			char temp_status[1025];
			memcpy(temp_status, data, data_size+1);
			temp_status[data_size-1] = '\r'; // restore CR
			temp_status[data_size+1] = '\0';

			// surprisingly, the status line is case-insensitive, according to a
			// strict reading of the spec
			// but the reference implementation treats the request line as case-sensitive
			// so we treat the response line aas case-sensitive as well
			char *token;
			if ((token = strtok(data, WHITESPACE)) == NULL || strcmp(token, "SIMPLE/1.0"))
				goto dump;
			if ((token = strtok(NULL, WHITESPACE)) == NULL || strcmp(token, "200"))
				goto dump;
			// normally we would ignore the reason phrase, but our assignment spec requires it
			if ((token = strtok(NULL, WHITESPACE)) == NULL || strcmp(token, "OK"))
				goto dump;
			if ((token = strtok(NULL, WHITESPACE)) != NULL)
				goto dump;

			status_ok = 1;
			continue;

dump:
			// not a proper 200 OK response, dump the entire response
			fputs(temp_status, stdout);
			fflush(stdout);
			bf_readall(bf, sfd);
			bf_writeall(bf, STDOUT_FILENO);
			status = EXIT_FAILURE;
			goto cleanup;
		} else { // regular headers
			char *token;
			if ((token = strtok(data, ":")) == NULL || strcasecmp(token, "content-length"))
				continue;
			if ((token = strtok(NULL, WHITESPACE)) == NULL)
				continue;

			char *endptr;
			errno = 0;
			resp_size = strtol(token, &endptr, 10);
			if (errno != 0 ||
				endptr == token || *endptr != '\0' ||
				resp_size < 0 || resp_size > MAX_BODY_SIZE) {
				fprintf(stderr, "invalid content-length\n");
				goto cleanup;
			}
		}
	}

	if (!status_ok || resp_size == -1) {
		fprintf(stderr, "failed parsing headers\n");
		goto cleanup;
	}

	if (bf_readn(bf, sfd, resp_size) == -1)
		goto cleanup;
	if (bf_writeall(bf, STDOUT_FILENO) == -1)
		goto cleanup;

	status = EXIT_SUCCESS;

cleanup:
	if (sfd != -1)
		close(sfd);
	if (bf != NULL)
		bf_free(bf);

	exit(status);
}

static int dial_server(const char *host, const char *port) {
	struct addrinfo *result, hints = (struct addrinfo) {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0,
		.ai_flags = AI_NUMERICSERV,
	};

	int res = getaddrinfo(host, port, &hints, &result);
	if (res) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
		return -1;
	}

	struct addrinfo *rp;
	int fd;
	for (rp = result; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd == -1)
			continue;
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == -1) {
			close(fd);
			continue;
		}
		break;
	}
	freeaddrinfo(result);
	if (!rp) {
		perror("connect");
		return -1;
	}

	return fd;
}

static int send_header(int sfd, const char *host, size_t size) {
	int res = dprintf(sfd,
		"POST message SIMPLE/1.0\r\n"
		"Host: %s\r\n"
		"Content-Length: %zd\r\n"
		"\r\n", host, size);
	if (res < 0) {
		perror("dprintf");
		return -1;
	}
	return 1;
}
