#define _DEFAULT_SOURCE
#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <sys/sendfile.h>
#include <sys/epoll.h>

/* Buffer library */

#define BF_BUFSIZE 4096
#define MIN(a, b) ((a) < (b) ? (a) : (b))

typedef struct buffer {
	size_t cap;
	size_t rp, wp;
	char data[];
} Buffer;

#define bf_size(buf) ((buf)->wp - (buf)->rp)
#define bf_space(buf) ((buf)->cap - (buf)->wp)
#define bf_fspace(buf) ((buf)->cap - bf_size(buf))
#define bf_reset(buf) ((buf)->rp = (buf)->wp = 0)

void _bf_pack(Buffer *buf) {
	memmove(buf->data, &buf->data[buf->rp], bf_size(buf));
	buf->wp = bf_size(buf);
	buf->rp = 0;
}

int _bf_readmore_into(Buffer *buf, int fd) {
	if (bf_fspace(buf) >= BF_BUFSIZE && bf_space(buf) < BF_BUFSIZE)
		_bf_pack(buf);

	ssize_t n = read(fd, &buf->data[buf->wp], MIN(bf_space(buf), BF_BUFSIZE));
	if(n < 0)
		return 0;
	if (!n) {
		// blatantly lie
		errno = ECONNRESET;
		return 0;
	}
	buf->wp += n;
	return 1;
}

Buffer *bf_new(size_t cap) {
	Buffer *buf = malloc(sizeof *buf + cap);
	if (!buf)
		return NULL;
	buf->cap = cap;
	bf_reset(buf);
	return buf;
}

void *bf_readline_into(Buffer *buf, int fd, ssize_t *size) {
	char *end;
	while ((end = memmem(&buf->data[buf->rp], bf_size(buf), "\r\n", 2)) == NULL) {
		if (!bf_fspace(buf)) {
			errno = ENOBUFS;
			return NULL;
		}
		errno = 0;
		if (!_bf_readmore_into(buf, fd))
			return NULL;
	}

	char *start = &buf->data[buf->rp];
	*size = end+2 - start;
	buf->rp += *size;
	return start;
}

int bf_writeall_to(Buffer *buf, int fd) {
	while (bf_size(buf)) {
		ssize_t n = write(fd, &buf->data[buf->rp], MIN(bf_size(buf), BF_BUFSIZE));
		if (n < 0)
			return 0;
		buf->rp += n;
	}
	return 1;
}

int bf_printf(Buffer *buf, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);

	size_t size = bf_space(buf);
	size_t n = vsnprintf(&buf->data[buf->wp], size, fmt, ap);
	if (n >= size && bf_fspace(buf) >= n) {
		_bf_pack(buf);
		size = bf_space(buf);
		n = vsnprintf(&buf->data[buf->wp], size, fmt, ap);
		if (n >= size) {
			va_end(ap);
			return 0;
		}
	}

	buf->wp += n;
	va_end(ap);
	return 1;
}

/* end Buffer library */

#define LISTEN_BACKLOG 128
#define MAX_EVENTS 128
#define MAX_CONNS 128
#define SEND_SIZE 4096

enum conn_phase {
	PHASE_NEW,
	PHASE_READ_HEADERS,
	PHASE_SEND_HEADERS,
	PHASE_SEND_BODY,
	PHASE_SEND_HEADERS_ONLY, // for errors
	PHASE_CLOSE,
};

struct conn_state {
	int efd;
	int conn_fd;
	// other fields not populated if conn_fd == sfd

	enum conn_phase phase;
	Buffer *rbuf, *wbuf;

	int host_seen;
	int close;

	char *req_fname;
	int req_fd;
	size_t req_size;
};

struct server_state {
	int efd;
	int conn_fd;
	int cur_conns;
};

static int start_server(int sfd);
static int listen_server(void);
static int accept_conns(struct server_state *server);

// handle a connection until error, completion, or i/o blocks
// 0 on close, -1 on error
static int handle_conn(struct conn_state *conn);
// advance the connection
static int advance_conn(struct conn_state *conn);

static int parse_reqline(struct conn_state *conn, char *buf, size_t size);
static int parse_header(struct conn_state *conn, char *buf, size_t size);
static int end_headers(struct conn_state *conn);

// constants
static const char http400[] = "HTTP/1.0 400 Bad Request\r\nConnection: close\r\n\r\n";
static const char http404[] = "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n";
static const char http500[] = "HTTP/1.0 500 Internal Server Error\r\nConnection: close\r\n\r\n";

// regex for parsing
// TODO: spaces in uri? urlencoding?
static const char pat_reqline[] = "^GET[[:space:]]+(/.*)[[:space:]]+HTTP/1\\.(0|1)[[:space:]]*\r\n$";
static const char pat_headers[] = "^([^:]+):[[:space:]]*(.*)\r\n$";
static regex_t preg_reqline, preg_headers;

static char *g_port;
static char *g_root = "./";

static int parse_args(int argc, char *argv[]) {
	int opt;
	while ((opt = getopt(argc, argv, "p:d:")) != -1) {
		switch (opt) {
			case 'p':
				g_port = optarg;
				break;
			case 'd':
				g_root = optarg;
				break;
			default:
				goto usage;
		}
	}

	if (!g_port)
		goto usage;

	int nport = atoi(g_port);
	if (nport <= 0 || nport > 65535)
		goto usage;

	return 1;

usage:
	fprintf(stderr, "Usage: %s -p port -d rootDirectory(optional)\n", argv[0]);
	return 0;
}

int main(int argc, char *argv[]) {
	sigaction(SIGPIPE, &(struct sigaction) { .sa_handler = SIG_IGN }, NULL);

	if (!parse_args(argc, argv))
		return EXIT_FAILURE;

	// compile regex
	if (regcomp(&preg_reqline, pat_reqline, REG_EXTENDED))
		return EXIT_FAILURE;
	if (regcomp(&preg_headers, pat_headers, REG_EXTENDED))
		return EXIT_FAILURE;

	int sfd = listen_server();
	if (sfd < 0)
		return EXIT_FAILURE;

	start_server(sfd);

	regfree(&preg_reqline);
	regfree(&preg_headers);
}

static int start_server(int sfd) {
	int efd = epoll_create1(0);
	if (efd < 0) {
		perror("epoll_create1");
		return 0;
	}

	struct server_state s_state = (struct server_state) {
		.efd = efd,
		.conn_fd = sfd,
		.cur_conns = 0,
	};
	struct epoll_event ev = {
		.events = EPOLLIN,
		.data = { .ptr = &(struct conn_state) {
			.efd = efd,
			.conn_fd = sfd,
		}},
	};
	if (epoll_ctl(efd, EPOLL_CTL_ADD, sfd, &ev) < 0) {
		perror("epoll_ctl");
		return 0;
	}

	struct epoll_event events[MAX_EVENTS];
	while (1) {
		int nfd = epoll_wait(efd, events, MAX_EVENTS, -1);
		if (nfd < 0) {
			perror("epoll_wait");
			return 0;
		}
		for (int i = 0; i < nfd; i++) {
			struct conn_state *conn = events[i].data.ptr;
			if (conn->conn_fd == sfd) {
				if (!accept_conns(&s_state))
					return 0;
			} else {
				int res = handle_conn(conn);
				if (res < 0)
					return 0;
				if (!res) {
					s_state.cur_conns--;
					free(conn);
				}
			}
		}

		ev.events = s_state.cur_conns < MAX_CONNS ? EPOLLIN : 0;
		if (epoll_ctl(efd, EPOLL_CTL_MOD, sfd, &ev) < 0) {
			perror("epoll_ctl");
			return 0;
		}
	}
	return 1;
}

static int listen_server(void) {
	struct addrinfo *result, hints = (struct addrinfo) {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM,
		.ai_protocol = 0,
		.ai_flags = AI_PASSIVE | AI_NUMERICHOST,
	};

	int res = getaddrinfo(NULL, g_port, &hints, &result);
	if (res) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(res));
		return -1;
	}

	struct addrinfo *rp;
	int fd;
	for (rp = result; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
		if (fd < 0)
			continue;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &(int) { 1 }, sizeof (int)) < 0) {
			close(fd);
			continue;
		}
		if (bind(fd, rp->ai_addr, rp->ai_addrlen) < 0) {
			close(fd);
			continue;
		}
		if (listen(fd, LISTEN_BACKLOG) < 0) {
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

static int accept_conns(struct server_state *server) {
	while (server->cur_conns < MAX_CONNS) {
		int cfd = accept4(server->conn_fd, NULL, NULL, SOCK_NONBLOCK);
		if (cfd < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 1;
			perror("accept");
			return 0;
		}

		struct conn_state *conn = malloc(sizeof *conn);
		if (!conn) {
			perror("malloc");
			close(cfd);
			return 0;
		}

		*conn = (struct conn_state) {
			.efd = server->efd,
			.conn_fd = cfd,

			.phase = PHASE_NEW,
			.rbuf = bf_new(BF_BUFSIZE),
			.wbuf = bf_new(BF_BUFSIZE),

			.host_seen = 0,
			.close = 0,
			.req_fname = NULL,
			.req_fd = -1,
			.req_size = 0,
		};
		if (!conn->rbuf || !conn->wbuf) {
			perror("malloc");
			if (conn->rbuf)
				free(conn->rbuf);
			if (conn->wbuf)
				free(conn->wbuf);
			free(conn);
			close(cfd);
			return 0;
		}

		struct epoll_event ev = {
			.events = EPOLLIN | EPOLLONESHOT,
			.data = { .ptr = conn },
		};
		if (epoll_ctl(server->efd, EPOLL_CTL_ADD, cfd, &ev) < 0) {
			perror("epoll_ctl");
			free(conn->rbuf);
			free(conn->wbuf);
			free(conn);
			close(cfd);
			return 0;
		}

		server->cur_conns++;
	}

	return 1;
}

static int handle_conn(struct conn_state *conn) {
	while (advance_conn(conn));

	struct epoll_event ev = (struct epoll_event) {
		.events = EPOLLONESHOT,
		.data = { .ptr = conn },
	};
	switch (conn->phase) {
		// reading states
		case PHASE_NEW: // fallthrough
		case PHASE_READ_HEADERS:
			ev.events |= EPOLLIN;
			break;

		// writing states
		case PHASE_SEND_HEADERS: // fallthrough
		case PHASE_SEND_BODY: // fallthrough
		case PHASE_SEND_HEADERS_ONLY:
			ev.events |= EPOLLOUT;
			break;

		// cleanup
		case PHASE_CLOSE:
			if (conn->req_fd >= 0)
				close(conn->req_fd);
			if (conn->req_fname)
				free(conn->req_fname);
			int res = epoll_ctl(conn->efd, EPOLL_CTL_DEL, conn->conn_fd, NULL);
			free(conn->rbuf);
			free(conn->wbuf);
			close(conn->conn_fd);
			if (res < 0) {
				perror("epoll_ctl");
				return -1;
			}
			return 0;
	}

	if (epoll_ctl(conn->efd, EPOLL_CTL_MOD, conn->conn_fd, &ev) < 0) {
		perror("epoll_ctl");
		return -1;
	}
	return 1;
}

static int advance_conn(struct conn_state *conn) {
	char *buf;
	ssize_t size;

	switch (conn->phase) {
		case PHASE_NEW:
			buf = bf_readline_into(conn->rbuf, conn->conn_fd, &size);
			if (!buf) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				if (errno == ENOBUFS)
					goto badRequest;
				perror("bf_readline_into");
				conn->phase = PHASE_CLOSE;
				return 0;
			}
			if (!parse_reqline(conn, buf, size))
				goto badRequest;
			break;

		case PHASE_READ_HEADERS:
			buf = bf_readline_into(conn->rbuf, conn->conn_fd, &size);
			if (!buf) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				if (errno == ENOBUFS)
					goto badRequest;
				perror("bf_readline_into");
				conn->phase = PHASE_CLOSE;
				return 0;
			}
			if (size == 2) {
				if (!end_headers(conn))
					goto badRequest;
			} else if (!parse_header(conn, buf, size))
				goto badRequest;
			break;

		case PHASE_SEND_HEADERS:
			if (!bf_writeall_to(conn->wbuf, conn->conn_fd)) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				perror("bf_writeall_to");
				conn->phase = PHASE_CLOSE;
				return 0;
			}
			conn->phase = PHASE_SEND_BODY;
			break;

		case PHASE_SEND_BODY:
			while (conn->req_size) {
				size = sendfile(conn->conn_fd, conn->req_fd, NULL, MIN(conn->req_size, SEND_SIZE));
				if (size < 0) {
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						return 0;
					perror("sendfile");
					conn->phase = PHASE_CLOSE;
					return 0;
				}
				conn->req_size -= size;
			}

			close(conn->req_fd);
			conn->req_fd = -1;

			if (conn->close) {
				conn->phase = PHASE_CLOSE;
				return 0;
			}

			conn->phase = PHASE_NEW;
			break;

		case PHASE_SEND_HEADERS_ONLY:
			if (!bf_writeall_to(conn->wbuf, conn->conn_fd)) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return 0;
				perror("bf_writeall_to");
			}
			conn->phase = PHASE_CLOSE;
			return 0;

		case PHASE_CLOSE:
			// not a real phase
			return 0;
	}

	return 1;

badRequest:
	conn->phase = PHASE_SEND_HEADERS_ONLY;
	conn->close = 1;
	bf_reset(conn->wbuf);
	bf_printf(conn->wbuf, "%s", http400);
	return 1;
}

static int parse_reqline(struct conn_state *conn, char *buf, size_t size) {
	// no nulls
	if (memchr(buf, '\0', size))
		return 0;

	// URI, version
	regmatch_t groups[3] = {{0, size}};
	if (regexec(&preg_reqline, buf, 3, groups, REG_STARTEND))
		return 0;

	conn->phase = PHASE_READ_HEADERS;
	// TODO: sanitize?

	conn->req_fname = malloc(strlen(g_root) + groups[1].rm_eo - groups[1].rm_so + 1);
	if (!conn->req_fname)
		return 0;

	strcpy(conn->req_fname, g_root);
	strncat(conn->req_fname, &buf[groups[1].rm_so], groups[1].rm_eo - groups[1].rm_so);
	conn->close = buf[groups[2].rm_so] == '0';

	return 1;
}

static int parse_header(struct conn_state *conn, char *buf, size_t size) {
	// no nulls
	if (memchr(buf, '\0', size))
		return 0;

	// key, value
	regmatch_t groups[3] = {{0, size}};
	if (regexec(&preg_headers, buf, 3, groups, REG_STARTEND))
		return 0;

	// terminate strings
	buf[groups[1].rm_eo] = '\0';
	buf[groups[2].rm_eo] = '\0';

	if (!strcasecmp(&buf[groups[1].rm_so], "Host")) {
		conn->host_seen = 1;
	} else if (!strcasecmp(&buf[groups[1].rm_so], "Connection")) {
		if (!strcasecmp(&buf[groups[2].rm_so], "Close"))
			conn->close = 1;
		else if (!strcasecmp(&buf[groups[2].rm_so], "Keep-Alive"))
			conn->close = 0;
		else
			return 0;
	}

	return 1;
}

static int end_headers(struct conn_state *conn) {
	if (!conn->host_seen)
		return 0;

	int fd = open(conn->req_fname, O_RDONLY);
	free(conn->req_fname);
	conn->req_fname = NULL;
	if (fd < 0) {
		perror("open");
		if (errno == ENOENT) {
			conn->phase = PHASE_SEND_HEADERS_ONLY;
			conn->close = 1;
			bf_reset(conn->wbuf);
			bf_printf(conn->wbuf, "%s", http404);
			return 1;
		}
		perror("open");
		goto internalError;
	}

	struct stat st;
	if (fstat(fd, &st) < 0) {
		perror("fstat");
		goto internalError;
	}
	if (!S_ISREG(st.st_mode)) {
		fprintf(stderr, "not a regular file\n");
		goto internalError;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size < 0) {
		perror("lseek");
		goto internalError;
	}
	if (lseek(fd, 0, SEEK_SET) < 0) {
		perror("lseek");
		goto internalError;
	}

	conn->phase = PHASE_SEND_HEADERS;
	conn->req_fd = fd;
	conn->req_size = size;
	conn->host_seen = 0;

	bf_printf(conn->wbuf, "HTTP/1.0 200 OK\r\n");
	bf_printf(conn->wbuf, "Connection: %s\r\n", conn->close ? "Close" : "Keep-Alive");
	bf_printf(conn->wbuf, "Content-Length: %zu\r\n", size);
	bf_printf(conn->wbuf, "\r\n");
	return 1;

internalError:
	if (fd >= 0)
		close(fd);

	conn->phase = PHASE_SEND_HEADERS_ONLY;
	conn->close = 1;
	bf_reset(conn->wbuf);
	bf_printf(conn->wbuf, "%s", http500);
	return 1; // we'd return 400 otherwise
}
