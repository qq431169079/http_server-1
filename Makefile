##need install jemalloc libary
CFLAG = -O2
all: threadpool.o
	gcc -Wall $(CFLAG)  ./bstrlib/bstrlib.c http_parser.c  httpd.c threadpool.o  -o httpd  -lpthread 
threadpool.o:
	gcc -Wall $(CFLAG)  -c threadpool.c -o threadpool.o
clean:
	rm -rf *.o httpd
