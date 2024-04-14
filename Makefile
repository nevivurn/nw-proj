CFLAGS := -D_FORTIFY_SOURCE=2 --std=c17 -Wall -Wextra -Werror -pedantic -Og -g

.PHONY: all
all: shttpd

.PHONY: check
check: shttpd
	$(MAKE) -C tests

.PHONY: clean
clean:
	make -C tests $@
	rm -f shttpd
