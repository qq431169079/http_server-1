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
#include "http_parser.h"

#define MAX_EVENTS 10240
#define IN_SIZE (10*1024)
#define PORT 8081
#define PROCESS_POOL_SIZEI 4 //处理线程池大小
#define PROCESS_QUEUE_SIZE 40 //处理队列大小
#define BUF_SIZE 8192
//#define BUF_SIZE 128  //for test


struct epoll_event events[MAX_EVENTS];
struct epoll_event ev;
static http_parser_settings settings;
//pthread_mutex_t count_lock = PTHREAD_MUTEX_INITIALIZER;
struct user_data{
    int epollfd;
    int sockfd;
    char in[IN_SIZE];
    int in_size;
    char out[IN_SIZE];
    http_parser *parser;
}userdata,*puserdata;

struct user_data* data_table[MAX_EVENTS+20];

struct http_request{
    const char *pbody;
    size_t  body_len;
    char url[255];
    char lastKey[50];
    int content_len;
    int end_msg;
};


int on_message_begin(http_parser* _) {
    (void)_;
    printf("\n***MESSAGE BEGIN***\n\n");
    return 0;
}

int on_headers_complete(http_parser* parser) {
    printf("\n***HEADERS COMPLETE***\n\n");
    return 0;
}

int on_message_complete(http_parser* parser) {
    printf("\n***MESSAGE COMPLETE***\n\n");
    struct http_request *p = (struct http_request *)parser->data;
    p->end_msg = 1;
    return 0;
}


int on_url(http_parser* parser, const char* at, size_t length) {
    struct http_request *p = (struct http_request *)parser->data;
    bzero(p->url,255);
    strncpy(p->url,at,length);
    printf("Url: %.*s\n", (int)length, at);
    return 0;
}

int on_header_field(http_parser* parser, const char* at, size_t length) {
    printf("Header field: %.*s\n", (int)length, at);
    struct http_request *p = (struct http_request *)parser->data;
    bzero(p->lastKey,50);
    strncpy(p->lastKey,at,length);
    return 0;
}

int on_header_value(http_parser* parser, const char* at, size_t length) {
    printf("Header value: %.*s\n", (int)length, at);
    struct http_request *p = (struct http_request *)parser->data;
    if(strcmp(p->lastKey,"content-length") == 0)
    {
        printf("Header value: %.*s\n", (int)length, at);
        char temp[20];
        bzero(temp,20);
        strncpy(temp,at,length); 
        p->content_len = atoi(temp);
    } 
    return 0;
}

int on_body(http_parser* parser, const char* at, size_t length) {
    printf("Body: %.*s\n", (int)length, at);
    struct http_request *p = (struct http_request *)parser->data;
    p->pbody = at;
    p->body_len = length;
    return 0;
}

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

void reset_oneshot(int epollfd, int fd)
{
    struct epoll_event event;
    event.data.fd = fd;
    event.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

int writen(int fd,char* buf,int len)
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
    return len - n;
}

void http_parse_request_cmd(const char *buf, int buflen, char *file_name, char *suffix)
{
    int length = 0;
    const char *begin, *end;

    /* 查找 URL 的开始位置 */
    begin = strchr(buf, (' '));
    begin += 1;

    /* 查找 URL 的结束位置 */
    end = strchr(begin, (' '));
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
    int fd=(int)(size_t)arg;
    struct user_data *pdata = data_table[fd];
    if(pdata == NULL)
    {
        printf("NULL\n");
        fflush(stdout);
        return NULL;
    }
    int epollfd = pdata->epollfd;
    int nread;
    size_t nparsed;
    char buf[BUF_SIZE];
    http_parser *parser = pdata->parser;
    struct http_request request;
    request.body_len = 0;
    request.content_len = -1; 
    request.end_msg = 0;
    parser->data = &request;
    int closed = 0;
    while(1)
    {
        nread=read(fd,buf,BUF_SIZE);
        printf("nread:%d\n",nread);
        if(nread > 0)
        {
            bcopy(buf,pdata->in+pdata->in_size,nread);
            pdata->in_size = pdata->in_size + nread;
            //printf("child thread tid = %u\n", pthread_self());
            //需要init
            http_parser_init(parser, HTTP_REQUEST); /* initialise parser */
            nparsed = http_parser_execute(parser, &settings, pdata->in, pdata->in_size); // 执行解析过程
            struct http_request *p = (struct http_request *)parser->data;
            printf("debug length:%d,%d\n",p->content_len, p->body_len);
            if(p->end_msg == 1)
            {
                char file_name[255]={0};
                //char suffix[16]="html";
                //http_parse_request_cmd((const char*)pdata->in,pdata->in_size,file_name,suffix);
                //判断文件是否存在
                if(strcmp(p->url,"/") == 0){
                    strcpy(file_name,"index.html");
                }
                else
                {
                    strcpy(file_name,p->url+1);
                }
                int status = 200;//状态码
                FILE *res_file;
                int read_len;
                int file_len = 0;
                printf("%s\n",file_name);
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
                //另一种获取文件大小的方法
                //参考 https://www.cnblogs.com/xuanyuanchen/p/6437357.html
                char sendbuf[10240]; //10K
                bzero(sendbuf,10240);
                if(file_len == 0)
                    sprintf(sendbuf,"HTTP/1.1 %d Not Found\r\nContent-type: text/html;charset=UTF-8\r\nContent-Length: %zu\r\n\r\n%s",status,strlen(tmp),tmp);
                else
                    sprintf(sendbuf,"HTTP/1.1 %d OK\r\nContent-type: text/html;charset=UTF-8\r\nContent-Length: %d\r\n\r\n",status,file_len);
                //printf("%s\n",sendbuf);
                int data_size = strlen(sendbuf);
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
                    fclose(res_file);
                }
                closed = 1;
                break;
            }
        }
        else if(nread < 0)
        {
            //资源繁忙的时候
            if (errno == EAGAIN) 
            {
                reset_oneshot(epollfd, fd);
                closed = 0;
            }
            else{
                printf("ERROR\n");
                closed = 1;      
            }

            break;
        }
        //client close
        else
        {
            closed = 1;
        }

    }

    if(closed == 1)
    {
        close(fd);
        epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, &ev);
        if(pdata)
        {
            data_table[fd] = NULL;
            free(pdata->parser);
            pdata->parser = NULL;
            free(pdata);
            pdata = NULL;
        }
        printf("client closed the connection\n");
        fflush(stdout);
    }


    return NULL;
}



int main(int argc,char *argv[]){
    signal(SIGCHLD, SIG_IGN);
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

    settings.on_message_begin = on_message_begin;
    settings.on_header_field = on_header_field;
    settings.on_header_value = on_header_value;
    settings.on_url = on_url;
    settings.on_body = on_body;
    settings.on_headers_complete = on_headers_complete;
    settings.on_message_complete = on_message_complete;
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
    /* 
     *       * 设置发送缓冲区大小 
     *            */ 
    int optlen;
    int snd_size = 32*1024;    /* 发送缓冲区大小为8K */ 
    int err;
    optlen = sizeof(snd_size); 
    err = setsockopt(listenfd, SOL_SOCKET, SO_SNDBUF, &snd_size, optlen); 
    if(err<0){ 
        printf("设置发送缓冲区大小错误\n"); 
    } 

    /* 
     *       * 设置接收缓冲区大小 
     *            */ 
    int rcv_size = 8*1024;    /* 接收缓冲区大小为8K */ 
    optlen = sizeof(rcv_size); 
    err = setsockopt(listenfd,SOL_SOCKET,SO_RCVBUF, (char *)&rcv_size, optlen); 
    if(err<0){ 
        printf("设置接收缓冲区大小错误\n"); 
    } 
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
            if (errno == EINTR)  
            {  
                continue;  
            }  
            perror("epoll_pwait");
            exit(EXIT_FAILURE);
        }

        for (index = 0; index < nfds; ++index)
        {
            fd = events[index].data.fd;
            if ((fd == listenfd) &&(events[index].events & EPOLLIN)) 
            {
                while ((conn_sock = accept(listenfd,(struct sockaddr *) &remote, &addrlen)) > 0) 
                {
                    setnonblocking(conn_sock);
                    //int reuse = 1;
                    //setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
                    //ev.events = EPOLLIN | EPOLLET; //测试没有多线程EPOLLONESHOT的bug
                    ev.events = EPOLLIN | EPOLLET | EPOLLONESHOT;
                    ev.data.fd= conn_sock;
                    if (epoll_ctl(epfd, EPOLL_CTL_ADD, conn_sock,&ev) == -1) 
                    {
                        perror("epoll_ctl: add");
                        exit(EXIT_FAILURE);
                    }
                    struct user_data *pdata = (struct user_data *)malloc(sizeof(struct user_data));
                    pdata->epollfd = epfd;
                    pdata->sockfd = conn_sock;
                    http_parser *parser = (http_parser*)malloc(sizeof(http_parser)); // 分配一个http_parser
                    pdata->parser = parser;
                    bzero(pdata->in,IN_SIZE);
                    bzero(pdata->out,IN_SIZE);
                    pdata->in_size = 0;
                    data_table[conn_sock] = pdata;

                }
                if (conn_sock == -1) 
                {
                    if (errno != EAGAIN && errno != ECONNABORTED 
                            && errno != EPROTO && errno != EINTR) 
                        perror("accept error");
                }
                continue;
            } 

            else if (events[index].events & EPOLLIN) 
            {
                printf("epoll in....\n");
                threadpool_add_job(th_pool,thread_process,(void*)(size_t)fd);
            }
            else if (events[index].events & EPOLLOUT)
            {

            }
        }
    }
    return 0;
}

