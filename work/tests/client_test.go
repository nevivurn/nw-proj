package main

import (
	"bytes"
	"context"
	"io"
	"net"
	"os"
	"os/exec"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func runClient(t *testing.T, host string, port int, in io.Reader) (string, string, error) {
	t.Helper()

	cmd := exec.Command(os.Getenv("CLIENT_BIN"), "-s", host, "-p", strconv.Itoa(port))
	stdout := bytes.Buffer{}
	stderr := bytes.Buffer{}
	cmd.Stdin = in
	cmd.Stdout = &stdout
	cmd.Stderr = &stderr

	err := cmd.Run()
	return stdout.String(), stderr.String(), err
}

func listenServer(t *testing.T) (net.Listener, string, int) {
	t.Helper()

	if os.Getenv("CLIENT_BIN") == "" {
		t.Skipf("CLIENT_BIN not set")
	}

	ln, err := net.Listen("tcp", "localhost:0")
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	t.Cleanup(func() { ln.Close() })

	ln.(*net.TCPListener).SetDeadline(time.Now().Add(timeout))

	addr := ln.Addr().(*net.TCPAddr)
	return ln, addr.IP.String(), addr.Port
}

type clientTest struct {
	name       string
	input      string
	req        string
	resp       string
	output     string
	shouldFail bool
}

func (tc clientTest) run(t *testing.T) {
	t.Helper()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	wg := sync.WaitGroup{}
	ln, host, port := listenServer(t)

	var stdout, stderr string
	var runErr error
	wg.Add(1)
	go func() {
		defer cancel()
		defer wg.Done()
		defer ln.Close()
		stdout, stderr, runErr = runClient(t, host, port, strings.NewReader(tc.input))
		if runErr != nil && !tc.shouldFail {
			t.Logf("client failed: %v", runErr)
		}
	}()

	c, err := ln.Accept()
	if err != nil {
		t.Fatalf("accept failed: %v", err)
	}
	defer c.Close()
	c.SetDeadline(time.Now().Add(timeout))

	context.AfterFunc(ctx, func() { c.Close() })

	buf := make([]byte, len(tc.req))
	if _, err := io.ReadFull(c, buf); err != nil {
		t.Fatalf("read failed: %v", err)
	}

	if string(buf) != tc.req {
		t.Fatalf("request mismatch: got %q, want %q", buf, tc.req)
	}

	wg.Add(1)
	go func() {
		defer cancel()
		defer wg.Done()
		bytes.NewBufferString(tc.resp).WriteTo(c)
	}()
	wg.Wait()

	if stdout != tc.output {
		t.Fatalf("output mismatch: got %q, want %q", stdout, tc.output)
	}
	if (runErr != nil) != tc.shouldFail {
		t.Fatalf("expected failure: %t; want %v", tc.shouldFail, runErr)
	}

	if stderr != "" {
		t.Log(strings.TrimSpace(stderr))
	}
}

func TestClient(t *testing.T) {
	tests := []clientTest{
		{
			name:   "basic",
			input:  "hello, world!",
			req:    formatValidRequest(t, "hello, world!"),
			resp:   formatValidResponse(t, "hello, world!"),
			output: "hello, world!",
		},
		{
			name:   "binary",
			input:  "\r\nhello, \x00world!\r\n\r\n",
			req:    formatValidRequest(t, "\r\nhello, \x00world!\r\n\r\n"),
			resp:   formatValidResponse(t, "\r\nhello, \x00world!\r\n\r\n"),
			output: "\r\nhello, \x00world!\r\n\r\n",
		},
		{
			name:   "large",
			input:  strings.Repeat("a", 10<<20),
			req:    formatValidRequest(t, strings.Repeat("a", 10<<20)),
			resp:   formatValidResponse(t, strings.Repeat("a", 10<<20)),
			output: strings.Repeat("a", 10<<20),
		},
		{
			name:   "large-input-overrun",
			input:  strings.Repeat("a", 10<<20) + "overrun!",
			req:    formatValidRequest(t, strings.Repeat("a", 10<<20)),
			resp:   formatValidResponse(t, strings.Repeat("a", 10<<20)),
			output: strings.Repeat("a", 10<<20),
		},
		{
			name:   "different-response",
			input:  "hello",
			req:    formatValidRequest(t, "hello"),
			resp:   formatValidResponse(t, "byeee"),
			output: "byeee",
		},

		{
			name:  "whitespace",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0\t200\tOK \t ",
				"Host: \t127.0.0.1",
				"Content-Length:5\r\n",
				"hello",
			}, "\r\n"),
			output: "hello",
		},
		{
			name:  "CaNcErcAsE",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				"hOsT: 127.0.0.1",
				"cOnTeNt-lEnGtH: 5\r\n",
				"hello",
			}, "\r\n"),
			output: "hello",
		},

		{
			name:  "extra-headers",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				"extra: headers",
				"Content-Length: 5",
				"this is still a valid header in theory",
				"Host: 127.0.0.1",
				"final header\r\n",
				"hello",
			}, "\r\n"),
			output: "hello",
		},

		{
			name:       "bad-request",
			input:      "hello",
			req:        formatValidRequest(t, "hello"),
			resp:       formatBadRequestResponse(t),
			output:     formatBadRequestResponse(t),
			shouldFail: true,
		},

		{
			name:       "invalid-response",
			input:      "hello",
			req:        formatValidRequest(t, "hello"),
			resp:       "wtf?\r\n",
			output:     "wtf?\r\n",
			shouldFail: true,
		},

		{
			name:  "invalid-statusline/version",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"simple/1.0 200 OK",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			output: strings.Join([]string{
				"simple/1.0 200 OK",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			shouldFail: true,
		},
		{
			name:  "invalid-statusline/code",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"simple/1.0 201 OK",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			output: strings.Join([]string{
				"simple/1.0 201 OK",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			shouldFail: true,
		},
		{
			name:  "invalid-statusline/reason",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"simple/1.0 200 NO",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			output: strings.Join([]string{
				"simple/1.0 200 NO",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			shouldFail: true,
		},

		{
			name:  "leading-whitespace/statusline",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				" SIMPLE/1.0 200 OK",
				"Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			output:     "",
			shouldFail: true,
		},
		{
			name:  "leading-whitespace/header",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				" Host: 127.0.0.1",
				"Content-Length: 5\r\n",
				"hello",
			}, "\r\n"),
			output:     "",
			shouldFail: true,
		},

		{
			name:  "invalid-content-length/missing",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				"some: header\r\n",
				"hello",
			}, "\r\n"),
			output:     "",
			shouldFail: true,
		},
		{
			name:  "invalid-content-length/neg",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				"Content-Length: -1\r\n",
				"hello",
			}, "\r\n"),
			output:     "",
			shouldFail: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}

// Ensure the client sends the correct Host header
func TestClientHost(t *testing.T) {
	ln, _, port := listenServer(t)

	wg := sync.WaitGroup{}

	buf := bytes.Buffer{}
	wg.Add(1)
	go func() {
		defer wg.Done()
		c, err := ln.Accept()
		if err != nil {
			t.Logf("accept failed: %v", err)
			return
		}
		defer c.Close()

		// just depend on the timeout, we don't bother responding
		c.SetDeadline(time.Now().Add(time.Second))
		if _, err := buf.ReadFrom(c); err != nil {
			t.Logf("read failed: %v", err)
		}
	}()

	runClient(t, "localhost", port, strings.NewReader("hello"))
	wg.Wait()

	want := strings.Join([]string{
		"POST message SIMPLE/1.0",
		"Host: localhost",
		"Content-Length: 5\r\n",
		"hello",
	}, "\r\n")
	if got := buf.String(); got != want {
		t.Fatalf("request mismatch: got %q, want %q", got, want)
	}
}

func TestClientOpinionated(t *testing.T) {
	if os.Getenv("TEST_OPINIONATED") == "" {
		t.Skipf("TEST_OPINIONATED is not set, skipping opinionated tests")
	}

	tests := []clientTest{
		{ // spec guarantees that the Content-Length matches the input, this is technically invalid
			name:  "invalid-content-length/huge",
			input: "hello",
			req:   formatValidRequest(t, "hello"),
			resp: strings.Join([]string{
				"SIMPLE/1.0 200 OK",
				"Content-Length: 10485761\r\n",
				strings.Repeat("a", 10<<20+1),
			}, "\r\n"),
			output:     "",
			shouldFail: true,
		},

		{ // the test harness ensures extraneous writes won't hang the server
			name:   "response-overrun",
			input:  strings.Repeat("a", 10<<20),
			req:    formatValidRequest(t, strings.Repeat("a", 10<<20)),
			resp:   formatValidResponse(t, strings.Repeat("a", 10<<20)) + "overrun!",
			output: strings.Repeat("a", 10<<20),
		},
		{ // the test server closes the connection, the client should not hang
			name:       "response-underrun",
			input:      strings.Repeat("a", 10<<20),
			req:        formatValidRequest(t, strings.Repeat("a", 10<<20)),
			resp:       func(s string) string { return s[:len(s)-1] }(formatValidResponse(t, strings.Repeat("a", 10<<20))),
			output:     "",
			shouldFail: true,
		},
	}
	for _, tc := range tests {
		t.Run(tc.name, tc.run)
	}
}
