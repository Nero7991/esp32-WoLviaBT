CC ?= gcc
CFLAGS = -std=c99 -Wall -Wextra -Werror -I../include

test: test_ota_util
	./test_ota_util

test_ota_util: test_ota_util.c ../ota_util.c ../include/ota_util.h
	$(CC) $(CFLAGS) -o $@ test_ota_util.c ../ota_util.c

clean:
	rm -f test_ota_util

.PHONY: test clean
