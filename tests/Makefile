.PHONY: check
check: data
	../shttpd -p 8080 -d testdata & \
		SERVER_HOST=localhost SERVER_PORT=8080 go test $(TEST_ARGS); \
		exit=$$?; \
		kill \%1 && wait; exit $$exit

.PHONY: data
data: testdata/hello testdata/1 testdata/2 testdata/3 testdata/4 testdata/5 testdata/huge-zeroes

testdata:
	mkdir -p testdata

testdata/hello: testdata
	echo -n "Hello, World!" > $@

testdata/1: testdata
	echo -n "1" > $@
testdata/2: testdata
	echo -n "2" > $@
testdata/3: testdata
	echo -n "3" > $@
testdata/4: testdata
	echo -n "4" > $@
testdata/5: testdata
	echo -n "5" > $@

testdata/huge-zeroes: testdata
	head -c $$(( 4 << 30 )) /dev/zero > $@

.PHONY: clean
clean:
	rm -rf testdata
