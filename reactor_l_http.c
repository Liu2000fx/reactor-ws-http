#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <pthread.h>
#include <sys/stat.h>
#include <sys/sendfile.h>

#define EPOLLLEN 1024
#define BUFLEN 1024
#define HTTP_METHOD_GET         0
#define HTTP_METHOD_POST        1
#define HTTP_WEBSERVER_HTML_ROOT    "html"
int e = 0;   // epoll tree


struct sockitem {
    int fd;
    int (*callback)(int fd, int events, char* arg);
    char recvbuf[BUFLEN];
    char sendbuf[BUFLEN];

    int recvlen;
    int sendlen;

    //http param
    int method; //
    char resource[BUFLEN];       // 资源文件路径
    int ret_code;
    char Content_Type[512];
};


int send_cb(int fd, int events, char* arg);
int recv_cb(int fd, int events, char* arg);
int connect_coute(int fd, int events, char* arg);

int readline(char *allbuf, int idx, char *linebuf) {
    uint len = strlen(allbuf);
    for (; idx < len; idx++) {
        if (allbuf[idx] == '\r' && allbuf[idx + 1] == '\n') {
            return idx + 2;
        }
        else {
            *(linebuf++) = allbuf[idx];
        }
    }
    return -1;
}


int http_response(struct sockitem *ev) {
    if (ev == NULL) return -1;
    memset(ev->recvbuf, 0, BUFLEN);
    int filefd = open(ev->resource, O_RDONLY);
    if (filefd == -1) { // return 404
        ev->ret_code = 404;
        ev->sendlen = sprintf(ev->sendbuf,
                             "HTTP/1.1 404 Not Found\r\n"
                             "Content-Type: %s;charset=utf-8\r\n"
                             "Content-Length: 83\r\n\r\n"
                             "<html><head><title>404 Not Found</title></head><body><H1>404</H1></body></html>\r\n\r\n",
                             ev->Content_Type);
    }
    else {
        struct stat stat_buf;
        fstat(filefd, &stat_buf);
        close(filefd);

        if (S_ISDIR(stat_buf.st_mode)) {
            ev->ret_code = 404;
            ev->sendlen = sprintf(ev->sendbuf,
                                 "HTTP/1.1 404 Not Found\r\n"
                                 "Content-Type: %s;charset=utf-8\r\n"
                                 "Content-Length: 83\r\n\r\n"
                                 "<html><head><title>404 Not Found</title></head><body><H1>404</H1></body></html>\r\n\r\n",
                                 ev->Content_Type);
        }
        else if (S_ISREG(stat_buf.st_mode)) {
            ev->ret_code = 200;
            ev->sendlen = sprintf(ev->sendbuf,
                                 "HTTP/1.1 200 OK\r\n"
                                 "Content-Type: %s;charset=utf-8\r\n"
                                 "Content-Length: %ld\r\n\r\n",
                                 ev->Content_Type, stat_buf.st_size);
        }
    }
    return ev->sendlen;
}


int send_cb(int fd, int events, char* arg) {
    
    struct sockitem* it = (struct sockitem*)arg;

    http_response(it);
    printf("\n[socket fd=%d count=%d]\n%s", fd, it->sendlen, it->sendbuf);
    int ret = send(fd, it->sendbuf, it->sendlen, 0);
    if(ret < 0) {
        perror("send");
        return -1;
    }

    if (it->ret_code == 200) {
            int filefd = open(it->resource, O_RDONLY);
            struct stat stat_buf;
            fstat(filefd, &stat_buf);
            off_t offset = 0;
            while (offset != stat_buf.st_size) {
                int n = sendfile(fd, filefd, &offset, (stat_buf.st_size - offset));
                if (n == -1 && errno == EAGAIN) {
                    usleep(5000);
                    continue;
                }
            }
            printf("[resource: %s  count:%ld]\r\n", it->resource, offset);
            close(filefd);
        }
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    it->callback = recv_cb;
    ev.data.ptr = it;

    epoll_ctl(e, EPOLL_CTL_MOD, fd, &ev);   // 发送就绪
    

    return ret;
}
int http_request(struct sockitem *it) {
    char line_buffer[1024] = {0};
    readline(it->recvbuf, 0, line_buffer);
    if (strstr(line_buffer, "GET")) {
        it->method = HTTP_METHOD_GET;
        //uri
        int i = 0;
        while (line_buffer[sizeof("GET ") + i] != ' ') i++;
        line_buffer[sizeof("GET ") + i] = '\0';
        sprintf(it->resource, "./%s/%s", HTTP_WEBSERVER_HTML_ROOT, line_buffer + sizeof("GET "));
        //Content-Type
        if (strstr(line_buffer + sizeof("GET "), ".")) {
            char *type = strchr(line_buffer + sizeof("GET "), '.') + 1;
            if (strcmp(type, "html") == 0 || strcmp(type, "css") == 0) {
                sprintf(it->Content_Type, "text/%s", type);
            }
            else if (strcmp(type, "jpg") == 0 || strcmp(type, "png") == 0 || strcmp(type, "ico") == 0) {
                sprintf(it->Content_Type, "image/%s", type);
            }
        }
        else {
            sprintf(it->Content_Type, "text/html");
        }
    }
}


int recv_cb(int fd, int events, char* arg) {
    struct sockitem *si = (struct sockitem*)arg;
    int ret = recv(fd, si->recvbuf, si->recvlen, 0);
    if(ret < 0) {
        if(errno == EAGAIN || errno == EWOULDBLOCK) {
            perror("errno == EAGAIN");
            return 0;
        }

        close(fd);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;

        epoll_ctl(e, EPOLL_CTL_DEL, fd, &ev);
        free(si);

        
    }else if(ret == 0) {
        printf("cli %d disconnect!\n", fd);

        close(fd);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;

        epoll_ctl(e, EPOLL_CTL_DEL, fd, &ev);
        free(si);
    }else {
        printf("    cli info -->  [%d bite, fd %d]\n%s\n\n", ret, fd, si->recvbuf);
        http_request(si);
        struct epoll_event ev;
        ev.events = EPOLLOUT | EPOLLET;
        ev.data.fd = fd;

        si->fd = fd;
        si->callback = send_cb;
        si->recvlen = ret;
        memcpy(si->sendbuf, si->recvbuf, si->recvlen);
        si->sendlen = si->recvlen;
        ev.data.ptr = si;

        epoll_ctl(e, EPOLL_CTL_MOD, fd, &ev);   // 发送就绪
        
    }

    

    return 0;
}

int connect_coute(int fd, int events, char* arg) {
    
    struct sockaddr_in cliaddr;
    memset(&cliaddr, 0, sizeof(cliaddr));

    socklen_t len = sizeof(cliaddr);
    int clifd = accept(fd, (struct sockaddr*)&cliaddr, &len);
    if(clifd < 0) {
        perror("accept");
        return -1;
    }
    char ip[INET_ADDRSTRLEN] = {0};
    printf("cli connect --> ip: %s, port %d fd %d\n",inet_ntop(AF_INET, &cliaddr.sin_addr.s_addr, ip, sizeof(ip)), 
        ntohs(cliaddr.sin_port), clifd);

    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    // ev.data.fd = clifd;

    struct sockitem* it = (struct sockitem*)malloc(sizeof(struct sockitem));
    it->fd = clifd;
    it->callback = recv_cb;
    memset(it->recvbuf, 0, BUFLEN);
    memset(it->sendbuf, 0, BUFLEN);
    it->recvlen = BUFLEN;
    it->sendlen = BUFLEN;
    ev.data.ptr = it;

    epoll_ctl(e, EPOLL_CTL_ADD, clifd, &ev);

    return clifd;
}




int main(int argc, char* argv[]) {
    if(argc < 2) {
        printf("parm <port>\n");
        return -2;
    }

    int port = atoi(argv[1]);
    int ret;

    int confd = socket(AF_INET, SOCK_STREAM, 0);
    if(confd < 0) {
        perror("socket");
        return -1;
    }
    struct sockaddr_in  srvaddr;
    memset(&srvaddr, 0, sizeof(srvaddr));
    srvaddr.sin_family = AF_INET;
    srvaddr.sin_addr.s_addr = INADDR_ANY;
    srvaddr.sin_port = htons(port);

    ret = bind(confd, (struct sockaddr*)&srvaddr, sizeof(srvaddr));
    if(ret < 0) {
        perror("bind");
        return -1;
    }
    if(listen(confd, 5) < 0) {
        perror("listen");
        return -1;
    };

    e = epoll_create(EPOLLLEN);
    struct epoll_event ev, events[EPOLLLEN] = {0};

    struct sockitem* item = (struct sockitem*)malloc(sizeof(struct sockitem));
    item->fd = confd;
    item->callback = connect_coute;
    memset(item->recvbuf, 0, BUFLEN);
    memset(item->sendbuf, 0, BUFLEN);
    item->recvlen = BUFLEN;
    item->sendlen = BUFLEN;
    // ev.data.fd = confd;         // 可以不写了，这个用不上了
    ev.events = EPOLLIN;
    ev.data.ptr = item;

    epoll_ctl(e, EPOLL_CTL_ADD, confd, &ev);

    while(1) {

        int nready = epoll_wait(e, events, EPOLLLEN, -1);
        if(nready < 0) {
            return -2; 
        }

        for(int i = 0; i < nready; i++) {
#if 0
            if(events[i].events & EPOLLIN) {
                // 连接请求
                struct sockitem* it = (struct sockitem*)events[i].data.ptr;
                it->callback(it->fd, events[i].events, (char *)it);

            }else if (events[i].events & EPOLLOUT) {
                struct sockitem* it = (struct sockitem*)events[i].data.ptr;
                it->callback(it->fd, events[i].events, (char *)it);
            }
#elif 1

            struct sockitem* it = (struct sockitem*)events[i].data.ptr;
            it->callback(it->fd, events[i].events, (char *)it);
#endif
        }

    }

}

