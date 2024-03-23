.PHONY: check
check:
	../server -p 8080 & \
		TEST_OPINIONATED=1 CLIENT_BIN=../client SERVER_HOST=localhost SERVER_PORT=8080 go test -v; \
		kill \%1
