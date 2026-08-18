CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Iinclude
BUILD := build

.PHONY: all clean test sanitize fuzz fuzz-build

all: $(BUILD)/cinderproxy

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cinderproxy: src/cinderproxy.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cinderproxy.c src/http.c -o $@

$(BUILD)/test_http: tests/test_http.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_http.c src/http.c -o $@

test: $(BUILD)/test_http
	./$(BUILD)/test_http

sanitize: | $(BUILD)
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined tests/test_http.c src/http.c -o $(BUILD)/test_http_san
	./$(BUILD)/test_http_san

fuzz-build: | $(BUILD)
	clang $(CPPFLAGS) -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined tests/fuzz_http.c src/http.c -o $(BUILD)/fuzz_http

fuzz: fuzz-build
	./$(BUILD)/fuzz_http -max_total_time=20

clean:
	rm -rf $(BUILD)
