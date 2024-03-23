CFLAGS := --std=c17 -Wall -Wextra -Werror -pedantic -Wno-unused-parameter -Og -g

.PHONY: all
all: client server sclient sserver readme.pdf

.PHONY: install
install: sclient.c sserver.c readme.pdf
	install -m644 sclient.c ../sclient.c
	install -m644 sserver.c ../sserver.c
	install -m644 readme.pdf ../readme.pdf

.PHONY: check
check: client server
	$(MAKE) -C tests $@

%.pdf: %.md
	pandoc $^ -o $@

client: client.c buffer.h buffer.c
	$(CC) $(CFLAGS) client.c buffer.c -o $@

server: server.c buffer.h buffer.c
	$(CC) $(CFLAGS) server.c buffer.c -o $@

sclient.c: client.c buffer.h buffer.c
	cat > $@ \
		<(sed '/#include "buffer.h"/,$$ d' client.c) \
		<(echo -e "\n// buffer.h") \
		buffer.h \
		<(echo -e "\n// buffer.c") \
		<(sed '/#include "buffer.h"/d' buffer.c) \
		<(echo -e "\n// client.c") \
		<(sed '0,/#include "buffer.h"/ d' client.c)

sserver.c: server.c buffer.h buffer.c
	cat > $@ \
		<(sed '/#include "buffer.h"/,$$ d' server.c) \
		<(echo -e "\n// buffer.h") \
		buffer.h \
		<(echo -e "\n// buffer.c") \
		<(sed '/#include "buffer.h"/d' buffer.c) \
		<(echo -e "\n// server.c") \
		<(sed '0,/#include "buffer.h"/ d' server.c)

.PHONY: clean
clean:
	rm -f client server sclient sserver sclient.c sserver.c report.pdf
