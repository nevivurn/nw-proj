#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "buffer.h"

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
