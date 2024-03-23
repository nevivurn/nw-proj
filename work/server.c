#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/epoll.h>

#include "buffer.h"

#define LISTEN_BACKLOG 128
#define MAX_EVENTS 64
#define MAX_CONNECTIONS 1000

#define MAX_BODY_SIZE (10 * 1024 * 1024)

// RFC 1945 defines LWS as [CRLF] 1*( SP | HT )
// but our assignment spec specifies isspace().
// Thus, we include all whitespace recognized by isspace() in POSIX locale.
// (include \f and \v)
#define WHITESPACE " \f\n\r\t\v"

enum cstate {
	RECV_REQUEST,
	RECV_HEADERS,
	RECV_BODY,
	SEND_HEADERS,
	SEND_BODY,
};

struct fd_state {
	int fd;

	// Populated if not the listening socket
	Buffer *buf, *hdrbuf;
	enum cstate state;
	int host_seen;
	ssize_t body_size;
};

static struct fd_state *new_fd_state(int fd);
static void free_fd_state(struct fd_state *state);

static struct fd_state *new_fd_state(int fd) {
	struct fd_state *state = malloc(sizeof *state);
	if (state == NULL) {
		perror("malloc");
		return NULL;
	}

	*state = (struct fd_state) {
		.fd = fd,
		.buf = bf_new(MAX_BODY_SIZE),
		.hdrbuf = bf_new(1024),
		.state = RECV_REQUEST,
		.host_seen = 0,
		.body_size = -1,
	};

	if (state->buf == NULL || state->hdrbuf == NULL) {
		perror("malloc");
		free_fd_state(state);
		return NULL;
	}

	return state;
}

static void free_fd_state(struct fd_state *state) {
	if (state->buf != NULL)
		bf_free(state->buf);
	if (state->hdrbuf != NULL)
		bf_free(state->hdrbuf);
	free(state);
}

static int listen_server(const char *port);
static int do_client(struct epoll_event ev, struct fd_state *state);

static void usage(char *argv[]) {
	fprintf(stderr, "usage: %s -p <port>\n", argv[0]);
	exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
	// Ignore SIGPIPE
	sigaction(SIGPIPE, &(struct sigaction) { .sa_handler = SIG_IGN }, NULL);

	// parse args
	int opt;
	char *port = NULL;
	while ((opt = getopt(argc, argv, "p:")) != -1) {
		switch (opt) {
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
	if (port == NULL)
		usage(argv);

	int status = EXIT_FAILURE;
	Buffer *bf = NULL;
	int sfd = -1, efd = -1;

	sfd = listen_server(port);
	if (sfd == -1)
		goto cleanup;

	efd = epoll_create1(0);
	if (efd == -1) {
		perror("epoll_create1");
		goto cleanup;
	}

	struct epoll_event ev = (struct epoll_event) {
		.events = EPOLLIN,
		.data.ptr = &(struct fd_state) { .fd = sfd },
	};
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
		perror("epoll_ctl g");
		goto cleanup;
	}

	int cur_connections = 0, accepting = 1;
	while (1) {
		struct epoll_event events[MAX_EVENTS];
		int nfd = epoll_wait(efd, events, MAX_EVENTS, -1);
		if (nfd == -1) {
			perror("epoll_wait");
			goto cleanup;
		}

		for (int i = 0; i < nfd; i++) {
			struct fd_state *state = events[i].data.ptr;
			if (state->fd == sfd) {
				if (cur_connections >= MAX_CONNECTIONS)
					continue;

				// listening socket, accept new connection
				int cfd = accept4(sfd, NULL, NULL, SOCK_NONBLOCK);
				if (cfd == -1) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						continue;
					perror("accept");
					goto cleanup;
				}
				cur_connections++;

				struct fd_state *cstate = new_fd_state(cfd);
				if (cstate == NULL) {
					close(cfd);
					cur_connections--;
					goto cleanup;
				}

				ev = (struct epoll_event) {
					.events = EPOLLIN,
					.data.ptr = cstate,
				};
				if (epoll_ctl(efd, EPOLL_CTL_ADD, cfd, &ev) == -1) {
					perror("epoll_ctl f");
					close(cfd);
					cur_connections--;
					free_fd_state(cstate);
					goto cleanup;
				}
				continue;
			}

			switch (do_client(events[i], state)) {
				case -2: // shutdown, wait for close
					shutdown(state->fd, SHUT_RDWR);
					ev = (struct epoll_event) {
						.events = EPOLLIN,
						.data.ptr = state,
					};
					if (epoll_ctl(efd, EPOLL_CTL_MOD, state->fd, &ev) == -1) {
						perror("epoll_ctl e");
						close(state->fd);
						cur_connections--;
						free_fd_state(state);
						goto cleanup;
					}
					break;

				case -1: // close and remove
					if (epoll_ctl(efd, EPOLL_CTL_DEL, state->fd, NULL) == -1) {
						perror("epoll_ctl d");
						close(state->fd);
						cur_connections--;
						free_fd_state(state);
						goto cleanup;
					}
					close(state->fd);
					cur_connections--;
					free_fd_state(state);
					break;

				case 1: // start writing
					ev = (struct epoll_event) {
						.events = EPOLLIN | EPOLLOUT,
						.data.ptr = state,
					};
					if (epoll_ctl(efd, EPOLL_CTL_MOD, state->fd, &ev) == -1) {
						perror("epoll_ctl c");
						close(state->fd);
						cur_connections--;
						free_fd_state(state);
						goto cleanup;
					}
					break;
			}
		}

		if (accepting && cur_connections >= MAX_CONNECTIONS) {
			if (epoll_ctl(efd, EPOLL_CTL_DEL, sfd, NULL) == -1) {
				perror("epoll_ctl b");
				goto cleanup;
			}
			accepting = 0;
		} else if (!accepting && cur_connections < MAX_CONNECTIONS) {
			ev = (struct epoll_event) {
				.events = EPOLLIN,
				.data.ptr = &(struct fd_state) { .fd = sfd },
			};
			if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) == -1) {
				perror("epoll_ctl a");
				goto cleanup;
			}
			accepting = 1;
		}
	}

cleanup:
	if (efd != -1)
		close(efd);
	if (sfd != -1)
		close(sfd);
	if (bf != NULL)
		bf_free(bf);

	exit(status);
}

static int listen_server(const char *port) {
	struct addrinfo *result, hints = (struct addrinfo) {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0,
		.ai_flags = AI_PASSIVE | AI_NUMERICSERV,
	};

	int res = getaddrinfo(NULL, port, &hints, &result);
	if (res) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
		return -1;
	}

	struct addrinfo *rp;
	int fd;
	for (rp = result; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
		if (fd == -1)
			continue;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &(int) { 1 }, sizeof(int)) == -1) {
			close(fd);
			continue;
		}
		if (bind(fd, rp->ai_addr, rp->ai_addrlen) == -1) {
			close(fd);
			continue;
		}
		if (listen(fd, LISTEN_BACKLOG) == -1) {
			close(fd);
			continue;
		}
		break;
	}
	freeaddrinfo(result);
	if (!rp) {
		perror("listen");
		return -1;
	}

	return fd;
}

static int validate_reqline(char *data, size_t size) {
	if (memchr(data, '\0', size) != NULL)
		return 0;
	data[size-2] = '\0'; // replace \r
	size -= 1;

	// No leading whitespace
	if (strchr(WHITESPACE, data[0]) != NULL) {
		return 0;
	}

	char *token;
	if ((token = strtok(data, WHITESPACE)) == NULL || strcmp(token, "POST"))
		return 0;
	if ((token = strtok(NULL, WHITESPACE)) == NULL || strcmp(token, "message"))
		return 0;
	if ((token = strtok(NULL, WHITESPACE)) == NULL || strcmp(token, "SIMPLE/1.0"))
		return 0;
	if ((token = strtok(NULL, WHITESPACE)) != NULL)
		return 0;

	return 1;
}

static int validate_header(struct fd_state *state, char *data, size_t size) {
	if (memchr(data, '\0', size) != NULL)
		return 0;
	data[size-2] = '\0'; // replace \r
	size -= 1;

	// No leading whitespace
	if (strchr(WHITESPACE, data[0]) != NULL) {
		return 0;
	}

	char *token;
	if ((token = strtok(data, ":")) == NULL)
		return 0;

	if (!strcasecmp(token, "host")) {
		state->host_seen = 1;
		return 1;
	}

	if (!strcasecmp(token, "content-length")) {
		if ((token = strtok(NULL, WHITESPACE)) == NULL)
			return 0;

		char *endptr;
		errno = 0;
		long req_size = strtol(token, &endptr, 10);
		if (errno != 0 ||
			endptr == token || *endptr != '\0' ||
			req_size < 0 || req_size > MAX_BODY_SIZE) {
			return 0;
		}
		state->body_size = req_size;
	}

	return 1;
}

static int discard(int fd) {
	char buf[1024];
	ssize_t n;
	while ((n = read(fd, buf, sizeof buf)) > 0);
	if (n == 0)
		return -1;
	if (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
		perror("read");
		return -1;
	}
	return 1;
}

#define RESP_BADREQUEST "SIMPLE/1.0 400 Bad Request\r\n\r\n"

static int do_client(struct epoll_event ev, struct fd_state *state) {
	int ok;

again:
	switch (state->state) {
		case RECV_REQUEST:
			// fallthrough
		case RECV_HEADERS:
			errno = 0;
			ok = bf_readuntil(state->buf, state->fd, "\r\n", 2);
			if (ok <= 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				return -1;
			}

			size_t data_size;
			char *data = bf_data_delim(state->buf, &data_size, "\r\n", 2);

			if (state->state == RECV_REQUEST) {
				if (!validate_reqline(data, data_size))
					goto badrequest;
				// request line good, read headers
				state->state = RECV_HEADERS;
				goto again;
			} else {
				// empty line, end of headers
				if (data_size == 2) {
					if (state->host_seen && state->body_size >= 0) {
						state->state = RECV_BODY;
						goto again;
					}
					goto badrequest;
				}
				// parse header
				if (!validate_header(state, data, data_size))
					goto badrequest;
				// keep reading headrs
				goto again;
			}

		badrequest:
			// bad request, discard buffer and send response
			state->state = SEND_BODY; // the entire response is in buf, not hdrbuf
			bf_reset(state->buf);
			bf_write_data(state->buf, RESP_BADREQUEST, sizeof RESP_BADREQUEST - 1);
			state->body_size = bf_size(state->buf);
			return 1;

		case RECV_BODY:
			errno = 0;
			ok = bf_readn(state->buf, state->fd, state->body_size);
			if (ok <= 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				return -1;
			}

			// done, start sending response
			state->state = SEND_HEADERS;
			bf_reset(state->hdrbuf);
			int hdr_size = snprintf(bf_data_raw(state->hdrbuf), bf_cap(state->hdrbuf),
				"SIMPLE/1.0 200 OK\r\n"
				"Content-Length: %zd\r\n"
				"\r\n", state->body_size);
			if ((size_t) hdr_size > bf_cap(state->hdrbuf)) {
				fprintf(stderr, "response header too big\n");
				return -1;
			}
			bf_seth_raw(state->hdrbuf, 0, hdr_size);

			return 1;

		case SEND_HEADERS:
			if (discard(state->fd) == -1)
				return -1;

			errno = 0;
			ok = bf_writeall(state->hdrbuf, state->fd);
			if (ok == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				return -1;
			}

			// done writing headers, start sending body
			state->state = SEND_BODY;
			return 0;

		case SEND_BODY:
			if (discard(state->fd) == -1)
				return -1;

			errno = 0;
			ok = bf_writen(state->buf, state->fd, state->body_size);
			state->body_size = bf_size(state->buf);
			if (ok == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				return -1;
			}
			// done writing
			return -2; // shutdown
	}

	return 0;
}
