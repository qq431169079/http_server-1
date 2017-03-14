##need install jemalloc libary
all: threadpool.o httpd.o
	gcc -Wall -g -O2 ./bstrlib/bstrlib.c -ljemalloc -lpthread  threadpool.o httpd.o  -o http_server
httpd.o:
	gcc -Wall -g -O2  -c httpd.c -o httpd.o
threadpool.o:
	gcc -Wall -g -O2  -c threadpool.c -o threadpool.o
clean:
	rm -rf *.o http_server
