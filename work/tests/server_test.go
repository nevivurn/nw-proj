package main

import (
	"bytes"
	"errors"
	"fmt"
	"io"
	"net"
	"os"
	"strings"
	"sync"
	"testing"
	"time"
)

const timeout = 5 * time.Second

func dialConn(t *testing.T) net.Conn {
	t.Helper()

	host := os.Getenv("SERVER_HOST")
	port := os.Getenv("SERVER_PORT")
	if host == "" || port == "" {
		t.Skipf("SERVER_HOST or SERVER_PORT not set")
	}

	c, err := net.Dial("tcp", fmt.Sprintf("%s:%s", host, port))
	if err != nil {
		t.Fatalf("connect failed: %v", err)
	}
	t.Cleanup(func() { c.Close() })

	return c
}

func sendRequest(t *testing.T, c net.Conn, req string) {
	t.Helper()
	c.SetDeadline(time.Now().Add(timeout))
	if _, err := bytes.NewBufferString(req).WriteTo(c); err != nil {
		// server may not read the entire request if it's invalid
		t.Logf("write failed: %v", err)
	}
}

func recvResponse(t *testing.T, c net.Conn) string {
	t.Helper()
	c.SetDeadline(time.Now().Add(timeout))
	buf := bytes.Buffer{}
	if _, err := buf.ReadFrom(c); err != nil {
		t.Fatalf("read failed: %v", err)
	}

	return buf.String()
}

func formatValidRequest(t *testing.T, body string) string {
	t.Helper()
	return fmt.Sprintf(
		"POST message SIMPLE/1.0\r\n"+
			"Host: 127.0.0.1\r\n"+
			"Content-Length: %d\r\n\r\n"+
			"%s", len(body), body)
}

func formatValidResponse(t *testing.T, body string) string {
	t.Helper()
	return fmt.Sprintf(
		"SIMPLE/1.0 200 OK\r\n"+
			"Content-Length: %d\r\n\r\n"+
			"%s", len(body), body)
}

func formatBadRequestResponse(t *testing.T) string {
	t.Helper()
	return "SIMPLE/1.0 400 Bad Request\r\n\r\n"
}

type serverTest struct {
	name    string
	request string
	want    string
}

func (tc serverTest) run(t *testing.T) {
	t.Helper()

	c := dialConn(t)
	sendRequest(t, c, tc.request)
	if got := recvResponse(t, c); got != tc.want {
		t.Errorf("got %q; want %q", got, tc.want)
	}
}

func TestServer(t *testing.T) {
	tests := []serverTest{
		{
			name:    "basic",
			request: formatValidRequest(t, "hello, world!"),
			want:    formatValidResponse(t, "hello, world!"),
		},
		{
			name:    "binary",
			request: formatValidRequest(t, "\r\nhello, \x00world!\r\n\r\n"),
			want:    formatValidResponse(t, "\r\nhello, \x00world!\r\n\r\n"),
		},
		{
			name:    "large",
			request: formatValidRequest(t, strings.Repeat("a", 10<<20)),
			want:    formatValidResponse(t, strings.Repeat("a", 10<<20)),
		},

		{
			name: "invalid-reqline/method",
			request: strings.Join([]string{
				"post msg SIMPLE/1.0",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
		{
			name: "invalid-reqline/path",
			request: strings.Join([]string{
				"POST MESSAGE SIMPLE/1.0",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
		{
			name: "invalid-reqline/version",
			request: strings.Join([]string{
				"post msg simple/1.0",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},

		{
			name: "whitespace",
			request: strings.Join([]string{
				"POST\tmessage\tSIMPLE/1.0 \t ",
				"Host: \t127.0.0.1\t",
				"Content-Length:5\t\r\n",
				"hello",
			}, "\r\n"),
			want: formatValidResponse(t, "hello"),
		},
		{
			name: "leading-whitespace/reqline",
			request: strings.Join([]string{
				" POST message SIMPLE/1.0",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
		{
			name: "leading-whitespace/header",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				" Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},

		{
			name: "extra-headers",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"extra: headers",
				"Content-Length: 5",
				"this is still a valid header in theory",
				"Host: 127.0.0.1",
				"final header\r\n",
				"hello",
			}, "\r\n"),
			want: formatValidResponse(t, "hello"),
		},
		{
			name: "missing-headers/host",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"ost: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
		{
			name: "missing-headers/content-length",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"Host: 127.0.0.1",
				"some: header\r\n\r\n",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},

		{
			name: "invalid-length/neg",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"Host: 127.0.0.1",
				"Content-Length: -1\r\n\r\n",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
		{
			name: "invalid-length/huge",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"Host: 127.0.0.1",
				"Content-Length: 10485761\r\n\r\n",
				strings.Repeat("a", 10<<20+1),
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},

		{
			name: "CaNcErcAsE",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"hOsT: LoCaLhOsT",
				"cOnTeNt-lEnGtH: 5\r\n",
				"hElLo",
			}, "\r\n"),
			want: formatValidResponse(t, "hElLo"),
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}

func TestServerReference(t *testing.T) {
	if os.Getenv("TEST_OPINIONATED") != "" {
		t.Skipf("TEST_OPINIONATED is set, skipping reference tests")
	}

	tests := []serverTest{
		{
			name:    "overrun",
			request: formatValidRequest(t, "hello, world!") + "overrun!",
			want:    "", // WTF?
		},
		{
			name:    "large-overrun",
			request: formatValidRequest(t, strings.Repeat("a", 10<<20)) + "overrun!",
			want:    formatBadRequestResponse(t),
		},
		{
			name:    "invalid-length/zero",
			request: formatValidRequest(t, ""),
			want:    formatBadRequestResponse(t),
		},
	}

	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}

func TestServerOpinionated(t *testing.T) {
	if os.Getenv("TEST_OPINIONATED") == "" {
		t.Skipf("TEST_OPINIONATED is not set, skipping opinionated tests")
	}

	tests := []serverTest{
		{ // reference returns empty for some reason
			name:    "overrun",
			request: formatValidRequest(t, "hello, world!") + "overrun!",
			want:    formatValidResponse(t, "hello, world!"),
		},
		{ // reference returns 400
			name:    "large-overrun",
			request: formatValidRequest(t, strings.Repeat("a", 10<<20)) + "overrun!",
			want:    formatValidResponse(t, strings.Repeat("a", 10<<20)),
		},
		{ // reference returns 400
			name:    "invalid-length/zero",
			request: formatValidRequest(t, ""),
			want:    formatValidResponse(t, ""),
		},

		{ // reference hangs
			name: "null-in-header",
			request: strings.Join([]string{
				"POST message SIMPLE/1.0",
				"Host: 127.0.0.1\x00",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			want: formatBadRequestResponse(t),
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}

func TestServerConcurrentConnections(t *testing.T) {
	conns := make([]net.Conn, 5)
	for i := range conns {
		conns[i] = dialConn(t)
	}

	for i, c := range conns {
		sendRequest(t, c, formatValidRequest(t, fmt.Sprintf("request %d", i)))
	}

	// read responses in reverse order
	for i := range conns {
		ii := len(conns) - 1 - i
		c := conns[ii]
		got := recvResponse(t, c)
		want := formatValidResponse(t, fmt.Sprintf("request %d", ii))
		if got != want {
			t.Errorf("got %q; want %q", got, want)
		}
	}
}

func TestServerManyConnections(t *testing.T) {
	if os.Getenv("TEST_OPINIONATED") == "" {
		t.Skipf("TEST_OPINIONATED is not set, skipping opinionated tests")
	}

	after := make(chan struct{})
	time.AfterFunc(time.Second, func() { close(after) })
	wg := sync.WaitGroup{}
	wg.Add(1000)

	for i := 0; i < 1000; i++ {
		i := i
		go func() {
			defer wg.Done()

			c := dialConn(t)
			sendRequest(t, c, formatValidRequest(t, fmt.Sprintf("request %d", i)))
			<-after

			got := recvResponse(t, c)
			want := formatValidResponse(t, fmt.Sprintf("request %d", i))
			if got != want {
				t.Errorf("got %q; want %q", got, want)
			}
		}()
	}

	wg.Wait()
}

func TestServerOneByte(t *testing.T) {
	c := dialConn(t)
	req := strings.NewReader(formatValidRequest(t, "hello, world!"))
	for req.Len() > 0 {
		b, _ := req.ReadByte()
		sendRequest(t, c, string(b))
	}

	// the kernel will probably buffer the response, but eh
	resp := bytes.Buffer{}
	buf := make([]byte, 1)
	for {
		n, err := c.Read(buf)
		resp.Write(buf[:n])
		if err != nil {
			if errors.Is(err, io.EOF) {
				break
			}
			t.Fatalf("read failed: %v", err)
		}
	}

	want := formatValidResponse(t, "hello, world!")
	if got := resp.String(); got != want {
		t.Errorf("got %q; want %q", got, want)
	}
}
