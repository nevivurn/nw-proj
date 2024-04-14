package main

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

const timeout = time.Second

func dialServer(t *testing.T) (*bufio.ReadWriter, func()) {
	t.Helper()

	host := os.Getenv("SERVER_HOST")
	port := os.Getenv("SERVER_PORT")
	if host == "" || port == "" {
		t.Skipf("SERVER_HOST and SERVER_PORT must be set")
	}

	c, err := net.Dial("tcp", fmt.Sprintf("%s:%s", host, port))
	if err != nil {
		t.Fatalf("dial failed: %v", err)
	}
	t.Cleanup(func() { c.Close() })
	c.SetDeadline(time.Now().Add(timeout))

	return bufio.NewReadWriter(bufio.NewReader(c), bufio.NewWriter(c)),
		func() { c.SetDeadline(time.Now().Add(timeout)) }
}

func readResponse(t *testing.T, r *bufio.Reader) *http.Response {
	t.Helper()

	resp, err := http.ReadResponse(r, nil)
	if err != nil {
		t.Fatalf("read failed: %v", err)
	}
	t.Cleanup(func() { resp.Body.Close() })

	return resp
}

func expectReaderEqual(t *testing.T, got, want io.Reader, setDeadline func()) {
	buf := make([]byte, 4096)
	cmpBuf := make([]byte, len(buf))

	for {
		setDeadline()
		nWant, err := want.Read(buf)
		if errors.Is(err, io.EOF) {
			if nWant == 0 {
				break
			}
		} else if err != nil {
			t.Fatalf("want read failed: %v", err)
		}

		_, err = io.ReadFull(got, cmpBuf[:nWant])
		if err != nil {
			t.Fatalf("got read failed: %v", err)
		}

		if !bytes.Equal(buf[:nWant], cmpBuf[:nWant]) {
			t.Fatalf("want %s; got %s",
				string(buf[:nWant]),
				string(cmpBuf[:nWant]))
		}
	}

	n, err := got.Read(buf)
	if !errors.Is(err, io.EOF) || n != 0 {
		t.Fatalf("want %d, %v; got %d, %v", 0, io.EOF, n, err)
	}
}

func expectResponse(t *testing.T, r *bufio.Reader, code int, keepAlive bool, body io.Reader, setDeadline func()) {
	t.Helper()

	resp := readResponse(t, r)
	defer resp.Body.Close()

	if resp.ProtoMinor != 0 {
		t.Errorf("want minor version %d; got %d", 0, resp.ProtoMinor)
	}
	if resp.StatusCode != code {
		t.Errorf("want status code %d; got %d", code, resp.StatusCode)
	}

	switch {
	case keepAlive && strings.EqualFold(resp.Header.Get("Connection"), "Keep-Alive"):
	case !keepAlive && strings.EqualFold(resp.Header.Get("Connection"), "Close"):
	default:
		t.Errorf("want connection close %v; got %v", !keepAlive, resp.Header.Get("Connection"))
	}

	expectReaderEqual(t, resp.Body, body, setDeadline)
}

func expectClose(t *testing.T, r *bufio.Reader) {
	t.Helper()

	_, err := r.Peek(1)
	if !errors.Is(err, io.EOF) {
		t.Errorf("want %v; got %v", io.EOF, err)
	}
}

func expectNotClose(t *testing.T, r *bufio.Reader) {
	t.Helper()

	_, err := r.Peek(1)
	if !errors.Is(err, os.ErrDeadlineExceeded) {
		t.Errorf("want %v; got %v", os.ErrDeadlineExceeded, err)
	}
}

type testCase struct {
	name       string
	request    string
	code       int
	keepAlive  bool
	body       string
	bodyReader io.Reader
}

func reqLines(lines ...string) string {
	return strings.Join(lines, "\r\n") + "\r\n\r\n"
}

func (tc testCase) run(t *testing.T) {
	t.Parallel()

	c, setDeadline := dialServer(t)

	c.WriteString(tc.request)
	fErrCh := make(chan error, 1)
	go func() { fErrCh <- c.Flush() }()

	if tc.bodyReader == nil {
		tc.bodyReader = strings.NewReader(tc.body)
	}

	expectResponse(t, c.Reader, tc.code, tc.keepAlive, tc.bodyReader, setDeadline)
	if tc.keepAlive {
		expectNotClose(t, c.Reader)
	} else {
		expectClose(t, c.Reader)
	}

	if err := <-fErrCh; err != nil {
		t.Fatalf("flush failed: %v", err)
	}
}

func openTestfile(t *testing.T, name string) *os.File {
	t.Helper()

	f, err := os.Open(filepath.Join("testdata", name))
	if err != nil {
		t.Fatalf("open failed: %v", err)
	}
	t.Cleanup(func() { f.Close() })

	return f
}

func TestSimple(t *testing.T) {
	tests := []testCase{
		{
			name:    "bad-request/method",
			request: reqLines("Get /hello HTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/method-post",
			request: reqLines("POST /hello HTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/protocol",
			request: reqLines("GET /hello Http/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/version",
			request: reqLines("GET /hello HTTP/2.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/host",
			request: reqLines("GET /hello HTTP/1.0", "Hos: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/uri",
			request: reqLines("GET hello HTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "bad-request/uri-space",
			request: reqLines("GET /hello o HTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},

		{
			name:      "keepalive-http10",
			request:   reqLines("GET /hello HTTP/1.0", "Host: localhost"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "keepalive-http11",
			request:   reqLines("GET /hello HTTP/1.1", "Host: localhost"),
			code:      http.StatusOK,
			keepAlive: true,
			body:      "Hello, World!",
		},
		{
			name:      "keepalive-http10-close",
			request:   reqLines("GET /hello HTTP/1.0", "Host: localhost", "Connection: Close"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "keepalive-http10-keep-alive",
			request:   reqLines("GET /hello HTTP/1.0", "Host: localhost", "Connection: Keep-Alive"),
			code:      http.StatusOK,
			keepAlive: true,
			body:      "Hello, World!",
		},
		{
			name:      "keepalive-http11-close",
			request:   reqLines("GET /hello HTTP/1.1", "Host: localhost", "Connection: Close"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "keepalive-http11-keep-alive",
			request:   reqLines("GET /hello HTTP/1.1", "Host: localhost", "Connection: Keep-Alive"),
			code:      http.StatusOK,
			keepAlive: true,
			body:      "Hello, World!",
		},

		{
			name:    "headers/case/host",
			request: reqLines("GET /hello HTTP/1.0", "hOsT: localhost"),
			code:    http.StatusOK,
			body:    "Hello, World!",
		},
		{
			name:      "headers/case/connection-keep-alive",
			request:   reqLines("GET /hello HTTP/1.0", "Host: localhost", "cOnNeCtIoN: kEeP-AlIvE"),
			code:      http.StatusOK,
			keepAlive: true,
			body:      "Hello, World!",
		},
		{
			name:      "headers/case/connection-close",
			request:   reqLines("GET /hello HTTP/1.1", "Host: localhost", "cOnNeCtIoN: cLoSe"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "headers/order",
			request:   reqLines("GET /hello HTTP/1.1", "Connection: Close", "Host: localhost"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "headers/space",
			request:   reqLines("GET /hello HTTP/1.1", "Connection\t :\t Close\t ", "Host \t: \tlocalhost \t"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name:      "headers/no-space",
			request:   reqLines("GET /hello HTTP/1.1", "Connection:Close", "Host:localhost"),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},
		{
			name: "headers/huge",
			request: reqLines(
				"GET /hello HTTP/1.1",
				strings.Repeat("A", 510)+": "+strings.Repeat("B", 510),
				"Connection: Close",
				"Host: localhost",
			),
			code:      http.StatusOK,
			keepAlive: false,
			body:      "Hello, World!",
		},

		{
			name:    "reqline/space",
			request: reqLines("GET\t /hello \tHTTP/1.0 \t ", "Host: localhost"),
			code:    http.StatusOK,
			body:    "Hello, World!",
		},
		{
			name:    "reqline/no-space/a",
			request: reqLines("GET/hello HTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},
		{
			name:    "reqline/no-space/b",
			request: reqLines("GET /helloHTTP/1.0", "Host: localhost"),
			code:    http.StatusBadRequest,
		},

		{
			name:       "huge-zeroes",
			request:    reqLines("GET /huge-zeroes HTTP/1.0", "Host: localhost"),
			code:       http.StatusOK,
			bodyReader: openTestfile(t, "huge-zeroes"),
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}

func TestConcurrent(t *testing.T) {
	c, setDeadline := dialServer(t)

	c.WriteString(reqLines("GET /1 HTTP/1.1", "Host: localhost"))
	c.WriteString(reqLines("GET /2 HTTP/1.1", "Host: localhost"))
	c.WriteString(reqLines("GET /3 HTTP/1.1", "Host: localhost"))
	c.WriteString(reqLines("GET /4 HTTP/1.1", "Host: localhost"))
	c.WriteString(reqLines("GET /5 HTTP/1.1", "Host: localhost"))
	c.WriteString(reqLines("GET /hello HTTP/1.0", "Host: localhost"))
	c.WriteString(reqLines("GET /huge-zeroes HTTP/1.1", "Host: localhost")) // should not be handled

	// no error check, server may not read the last request
	go func() { c.Flush() }()

	expectResponse(t, c.Reader, http.StatusOK, true, strings.NewReader("1"), setDeadline)
	expectResponse(t, c.Reader, http.StatusOK, true, strings.NewReader("2"), setDeadline)
	expectResponse(t, c.Reader, http.StatusOK, true, strings.NewReader("3"), setDeadline)
	expectResponse(t, c.Reader, http.StatusOK, true, strings.NewReader("4"), setDeadline)
	expectResponse(t, c.Reader, http.StatusOK, true, strings.NewReader("5"), setDeadline)
	expectResponse(t, c.Reader, http.StatusOK, false, strings.NewReader("Hello, World!"), setDeadline)
	expectClose(t, c.Reader)
}

func TestRoot(t *testing.T) {
	c, _ := dialServer(t)

	c.WriteString(reqLines("GET / HTTP/1.0", "Host: localhost"))

	fErrCh := make(chan error, 1)
	go func() { fErrCh <- c.Flush() }()

	// directory response is undefined, 4xx or 5xx all acceptable
	resp := readResponse(t, c.Reader)
	defer resp.Body.Close()

	if resp.StatusCode < 400 && resp.StatusCode >= 600 {
		t.Errorf("want 4xx or 5xx; got %d", resp.StatusCode)
	}
	expectClose(t, c.Reader)

	if err := <-fErrCh; err != nil {
		t.Fatalf("flush failed: %v", err)
	}
}
