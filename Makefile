CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Iinclude
FUZZ_CC ?= clang
FUZZ_TIME ?= 20
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
TLS_CPPFLAGS ?= $(if $(OPENSSL_PREFIX),-I$(OPENSSL_PREFIX)/include,)
TLS_LDFLAGS ?= $(if $(OPENSSL_PREFIX),-L$(OPENSSL_PREFIX)/lib,) -lssl -lcrypto
BUILD := build

.PHONY: all clean test integration sanitize fuzz fuzz-build tls benchmark benchmark-suite

all: $(BUILD)/cinderproxy

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/cinderproxy: src/cinderproxy.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cinderproxy.c src/http.c -o $@

$(BUILD)/cinderproxy-tls: src/cinderproxy.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(TLS_CPPFLAGS) -DCINDER_TLS $(CFLAGS) src/cinderproxy.c src/http.c -o $@ $(TLS_LDFLAGS)

tls: $(BUILD)/cinderproxy-tls

$(BUILD)/test_http: tests/test_http.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_http.c src/http.c -o $@

test: $(BUILD)/test_http
	./$(BUILD)/test_http

integration: $(BUILD)/cinderproxy
	python3 tests/test_integration.py

benchmark: $(BUILD)/cinderproxy
	python3 tools/benchmark.py

benchmark-suite: $(BUILD)/cinderproxy
	python3 tools/benchmark_suite.py

sanitize: | $(BUILD)
	$(CC) $(CPPFLAGS) -std=c11 -Wall -Wextra -Wpedantic -O1 -g -fsanitize=address,undefined tests/test_http.c src/http.c -o $(BUILD)/test_http_san
	./$(BUILD)/test_http_san

fuzz-build: | $(BUILD)
	$(FUZZ_CC) $(CPPFLAGS) -std=c11 -O1 -g -fsanitize=fuzzer,address,undefined tests/fuzz_http.c src/http.c -o $(BUILD)/fuzz_http

fuzz: fuzz-build
	./$(BUILD)/fuzz_http -max_total_time=$(FUZZ_TIME)

clean:
	rm -rf $(BUILD)
