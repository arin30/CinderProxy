CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Wpedantic -O2 -pthread
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L -Iinclude
BUILD := build

.PHONY: all clean test
all: $(BUILD)/cinderproxy
$(BUILD):
	mkdir -p $(BUILD)
$(BUILD)/cinderproxy: src/cinderproxy.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/cinderproxy.c src/http.c -o $@
$(BUILD)/test_http: tests/test_http.c src/http.c include/http.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_http.c src/http.c -o $@
test: $(BUILD)/test_http
	./$(BUILD)/test_http
clean:
	rm -rf $(BUILD)
