#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <errno.h> 

#include "threadpool.h"
#include "bstrlib/bstrlib.h"

#define MAX_EVENTS 10240
#define PORT 8864
#define PROCESS_POOL_SIZEI 4 //处理线程池大小
#define PROCESS_QUEUE_SIZE 40 //处理队列大小
#define BUF_SIZE 8192
//#define BUF_SIZE 128  //for test


struct epoll_event events[MAX_EVENTS];
struct epoll_event ev;

struct fds{
    int index;
    int epollfd;
    int sockfd;
};


//设置socket连接为非阻塞模式
void setnonblocking(int sockfd)
{
    int opts;

    opts = fcntl(sockfd, F_GETFL);
    if(opts < 0) {
        perror("fcntl(F_GETFL)\n");
        exit(1);
    }
    opts = (opts | O_NONBLOCK);
    if(fcntl(sockfd, F_SETFL, opts) < 0) {
        perror("fcntl(F_SETFL)\n");
        exit(1);
    }
}

ssize_t readn (int fd, void *buf, size_t num)
{
    ssize_t res;
    size_t n;
    char *ptr;


    n = num;
    ptr = buf;

    while ((res = read (fd, ptr, n)) >0) {
        ptr += res;
        n -= res;

    }

    if (res == -1 && errno != EAGAIN) {

        perror("read error");

    }
    return (num - n);
}


void writen(int fd,char* buf,int len)
{
    int n=len;
    int nwrite;
    while (n > 0) {
        nwrite = write(fd, buf + len - n, n);
        if (nwrite < n) {
            if (nwrite == -1 && errno != EAGAIN) {
                perror("write error");
            }
            break;
        }
        n -= nwrite;
    }
}

void http_parse_request_cmd(const char *buf, int buflen, char *file_name, char *suffix)
{
    int length = 0;
    char *begin, *end;
 
    /* 查找 URL 的开始位置 */
    begin = strchr(buf, ' ');
    begin += 1;

    /* 查找 URL 的结束位置 */
    end = strchr(begin, ' ');
    length = end - begin;

    /* 找到文件名的开始位置 */
    if ((*begin == '/') || (*begin == '\\'))
    {
        begin++;
        length--;
    }

    /* 得到文件名 */
    if (length > 0)
    {
        memcpy(file_name, begin, length);
        file_name[length] = 0;

        begin = strchr(file_name, '.');
        if (begin)
            strcpy(suffix, begin + 1);
    }
}


void* thread_process(void* arg)
{
    struct fds *pdata=(struct fds *)arg;
    int fd=pdata->sockfd;
    int epollfd = pdata->epollfd;
    int nread = 0;
    int total = 0;
    char buf[BUF_SIZE];
    bstring result = bfromcstr("");
    //printf("%s",result);
    while(1)
    {
        bzero(buf,BUF_SIZE);
        nread=readn(fd,buf,BUF_SIZE);
        if(nread > 0)
        {
            bcatcstr(result,buf);
            total += nread;
        }
        else if(nread < 0)
        {
            //资源繁忙的时候
            if (errno == EAGAIN) 
            {
                continue;
            }
        }
        else
        {
            break;
        }
    }
    printf("recive:%d\n",total);
    if(total > 0)
    {
        printf("%s",result->data);
        char file_name[255]="index.html";
        char suffix[16]="html";
        http_parse_request_cmd((const char*)result->data,total,file_name,suffix);
        fflush(stdout);
        //判断文件是否存在
        int status = 200;//状态码
        FILE *res_file;
        int read_len;
        int file_len = 0;
        res_file = fopen(file_name,"rb+");//使用二进制格式打开文件
        char * tmp = "404 error!";
        if(res_file == NULL)
        {
            status = 404;
        }
        else
        {
            fseek(res_file, 0, SEEK_END);
            file_len = ftell(res_file);
            fseek(res_file, 0, SEEK_SET);
        }

        char sendbuf[10240]; //10K
        bzero(sendbuf,10240);
        if(file_len == 0)
            sprintf(sendbuf,"HTTP/1.1 %d Not Found\r\nContent-type: text/html;charset=UTF-8\r\nContent-Length: %zu\r\n\r\n%s",status,strlen(tmp),tmp);
        else
            sprintf(sendbuf,"HTTP/1.1 %d OK\r\nContent-type: text/html;charset=UTF-8\r\nContent-Length: %d\r\n\r\n",status,file_len);
        //printf("%s\n",sendbuf);
        int data_size = strlen(sendbuf);
        //发送头部
        writen(fd,sendbuf,data_size);
        if(file_len > 0)
        {
            //发送Body
            do /* 发送文件, HTTP 的消息体 */
            {
                read_len = fread(buf, sizeof(char), BUF_SIZE, res_file);

                if (read_len > 0)
                {
                    data_size = send(fd, buf, read_len, 0);
                    file_len -= read_len;
                }
                else if(read_len == -1)
                {
                    if(errno != EAGAIN)
                    {
                      perror("write error");
                    }
                    else
                    {
                      continue;//继续循环
                    }

                }
                
            } while ((read_len > 0) && (file_len > 0));  
        }
    }
    bdestroy(result);
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
    close(fd);
    return NULL;
}


void reset_oneshot(int epollfd, int fd) 
{
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int main(int argc,char *argv[]){

    if(argc>1 && strcmp(argv[1],"daemon")==0)
    {
        pid_t pidfd = fork();
        if (pidfd < 0)
        {
            return (-1);
        }
        if (pidfd != 0)
        {
            exit(0);
        }
        setsid();
    }

    struct sockaddr_in local;
    int listenfd;
    int epfd;
    //struct epoll_event ev;
    int fd,nfds,conn_sock, index;
    socklen_t addrlen;
    struct sockaddr_in remote;
    //创建listen socket
    if( (listenfd = socket(AF_INET, SOCK_STREAM, 0)) < 0) 
    {
        perror("sockfd\n");
        exit(1);
    }
    setnonblocking(listenfd);
    bzero(&local, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(PORT);
    if( bind(listenfd, (struct sockaddr *) &local, sizeof(local)) < 0) 
    {
        perror("bind error\n");
        exit(1);
    }
    listen(listenfd, 511);//backlog参数

    //epfd = epoll_create(MAX_EVENTS);
    //新版本
    epfd=epoll_create1(0);
    if (epfd == -1) 
    {
        perror("epoll_create");
        exit(EXIT_FAILURE);
    }
    ev.events = EPOLLIN|EPOLLET;
    ev.data.fd=listenfd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, listenfd, &ev) == -1) 
    {
        perror("epoll_ctl: listen_sock");
        exit(EXIT_FAILURE);
    }

    //init thread
    struct threadpool* th_pool;
    th_pool=threadpool_init(PROCESS_POOL_SIZEI, PROCESS_QUEUE_SIZE);
    for (;;) 
    {
        nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (nfds == -1) 
        {
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (index = 0; index < nfds; ++index)
        {
            fd = events[index].data.fd;
            if (fd == listenfd) 
            {
                while ((conn_sock = accept(listenfd,(struct sockaddr *) &remote, &addrlen)) > 0) 
                {
                    setnonblocking(conn_sock);
                    ev.events = EPOLLIN | EPOLLET;
                    ev.data.fd= conn_sock;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock,&ev) == -1) 
                    {
                        perror("epoll_ctl: add");
                        exit(EXIT_FAILURE);
                    }
                }
                if (conn_sock == -1) 
                {
                    if (errno != EAGAIN && errno != ECONNABORTED 
                            && errno != EPROTO && errno != EINTR) 
                        perror("accept error");
                }
                continue;
            } 

            if (events[index].events & EPOLLIN) 
            {
                struct fds pdata;
                pdata.index = index;
                pdata.epollfd = epfd;
                pdata.sockfd = fd;
                threadpool_add_job(th_pool,thread_process,(void *)&pdata);
            }
        }
    }
    return 0;
}
