all: netfilter-test

netfilter-test.o: libnet.h netfilter-test.c

netfilter-test: netfilter-test.o
	gcc -o netfilter-test netfilter-test.o -lnetfilter_queue

clean:
	rm -f netfilter-test *.o