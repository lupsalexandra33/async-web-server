CC = gcc
CPPFLAGS = -DDEBUG -DLOG_LEVEL=LOG_DEBUG -I.
CFLAGS = -Wall -Wextra -g
LDLIBS = -laio

OBJS = aws.o utils/sock_util.o http-parser/http_parser.o

.PHONY: all clean

all: aws

aws: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

aws.o: aws.c aws.h utils/sock_util.h utils/debug.h utils/util.h utils/w_epoll.h http-parser/http_parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

utils/sock_util.o: utils/sock_util.c utils/sock_util.h utils/util.h utils/debug.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

http-parser/http_parser.o: http-parser/http_parser.c http-parser/http_parser.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

clean:
	rm -f aws $(OBJS)