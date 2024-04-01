CFLAGS := --std=c17 -Wall -Wextra -Werror -pedantic -Wno-unused-parameter -Og -g

.PHONY: all
all: shttpd

.PHONY: clean
clean:
	rm -f shttpd
