
// gcc epoll_websocket.c -o web -lcrypto

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

#include <openssl/sha.h>
#include <openssl/pem.h>
#include <openssl/bio.h>
#include <openssl/evp.h>

#define EPOLLLEN 1024
#define BUFLEN 1024
#define GUID 					"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
struct reactor* e = NULL;   // epoll tree


enum WEBSOCKETSTATUS {
    WB_INIT,
    WB_HANDSHAKE,
    WB_DATATRANSFROMER,
    WB_DATAEND,
};



struct sockitem {
    int fd;
    int (*callback)(int fd, int events, char* arg);
    char recvbuf[BUFLEN];
    char sendbuf[BUFLEN];

    int recvlen;
    int sendlen;

    int status; // 状态机
};

struct reactor {
    int epfd;
    struct epoll_event events[EPOLLLEN];
};



struct ws_ophdr {
	unsigned char opcode:4,
        rsv3:1,
        rsv2:1,
        rsv1:1,
        fin:1;
	unsigned char payload_length:7,
		mask:1;
} __attribute__ ((packed));

struct _nty_websocket_head_126 {
	unsigned short payload_length;
	char mask_key[4];
	unsigned char data[8];
} __attribute__ ((packed));

struct _nty_websocket_head_127 {

	unsigned long long payload_length;
	char mask_key[4];

	unsigned char data[8];
	
} __attribute__ ((packed));

typedef struct _nty_websocket_head_127 nty_websocket_head_127;
typedef struct _nty_websocket_head_126 nty_websocket_head_126;
typedef struct ws_ophdr nty_ophdr;






int send_cb(int fd, int events, char* arg);
int recv_cb(int fd, int events, char* arg);
int connect_coute(int fd, int events, char* arg);


int readline(char* allbuf,int level,char* linebuf) {    
	int len = strlen(allbuf);    

	for (;level < len; ++level)    {        
		if(allbuf[level]=='\r' && allbuf[level+1]=='\n')            
			return level+2;        
		else            
			*(linebuf++) = allbuf[level];    
	}    

	return -1;
}
int base64_encode(char *in_str, int in_len, char *out_str) {    
	BIO *b64, *bio;    
	BUF_MEM *bptr = NULL;    
	size_t size = 0;    

	if (in_str == NULL || out_str == NULL)        
		return -1;    

	b64 = BIO_new(BIO_f_base64());    
	bio = BIO_new(BIO_s_mem());    
	bio = BIO_push(b64, bio);
	
	BIO_write(bio, in_str, in_len);    
	BIO_flush(bio);    

	BIO_get_mem_ptr(bio, &bptr);    
	memcpy(out_str, bptr->data, bptr->length);    
	out_str[bptr->length-1] = '\0';    
	size = bptr->length;    

	BIO_free_all(bio);    
	return size;
}

// 握手
int handshark(struct sockitem *si, struct reactor *mainloop) {
	char linebuf[256];
	char sec_accept[32]; 
	int level = 0;
	unsigned char sha1_data[SHA_DIGEST_LENGTH+1] = {0};
	char head[BUFLEN] = {0};  

	do {        
		memset(linebuf, 0, sizeof(linebuf));        
		level = readline(si->recvbuf, level, linebuf); 

		if (strstr(linebuf,"Sec-WebSocket-Key") != NULL)        {   
			
			strcat(linebuf, GUID);    
			
			SHA1((unsigned char*)&linebuf+19,strlen(linebuf+19),(unsigned char*)&sha1_data);  
			
			base64_encode(sha1_data,strlen(sha1_data),sec_accept);           
			sprintf(head, "HTTP/1.1 101 Switching Protocols\r\n" \
				"Upgrade: websocket\r\n" \
				"Connection: Upgrade\r\n" \
				"Sec-WebSocket-Accept: %s\r\n" \
				"\r\n", sec_accept);            
          
			printf("\n\n\n");            

			memset(si->recvbuf, 0, BUFLEN);
            memset(si->sendbuf, 0, BUFLEN);
			memcpy(si->sendbuf, head, strlen(head)); // to send 
			si->sendlen = strlen(head);
            // to set epollout events
            struct epoll_event ev;
            ev.events = EPOLLOUT | EPOLLET;
            si->callback = send_cb;
            si->status = WB_DATATRANSFROMER;

            ev.data.ptr = si;

            epoll_ctl(e->epfd, EPOLL_CTL_MOD, si->fd, &ev);
			break;        
		}    

	} while((si->recvbuf[level] != '\r' || si->recvbuf[level+1] != '\n') && level != -1);    

	return 0;
}

void umask(char *data,int len,char *mask) {    
	int i;    
	for (i = 0;i < len;i ++)        
		*(data+i) ^= *(mask+(i%4));
}

char* decode_packet(char *stream, char *mask, int length, int *ret) {

	nty_ophdr *hdr =  (nty_ophdr*)stream;
	unsigned char *data = stream + sizeof(nty_ophdr);
	int size = 0;
	int start = 0;
	//char mask[4] = {0};
	int i = 0;

	//if (hdr->fin == 1) return NULL;

	if ((hdr->mask & 0x7F) == 126) {

		nty_websocket_head_126 *hdr126 = (nty_websocket_head_126*)data;
		size = hdr126->payload_length;
		
		for (i = 0;i < 4;i ++) {
			mask[i] = hdr126->mask_key[i];
		}
		
		start = 8;
		
	} else if ((hdr->mask & 0x7F) == 127) {

		nty_websocket_head_127 *hdr127 = (nty_websocket_head_127*)data;
		size = hdr127->payload_length;
		
		for (i = 0;i < 4;i ++) {
			mask[i] = hdr127->mask_key[i];
		}
		
		start = 14;

	} else {
		size = hdr->payload_length;

		memcpy(mask, data, 4);
		start = 6;
	}

	*ret = size;
	umask(stream+start, size, mask);

	return stream + start;
	
}


int encode_packet(char *buffer,char *mask, char *stream, int length) {

	nty_ophdr head = {0};
	head.fin = 1;
	head.opcode = 1;
	int size = 0;

	if (length < 126) {
		head.payload_length = length;
		memcpy(buffer, &head, sizeof(nty_ophdr));
		size = 2;
	} else if (length < 0xffff) {
		nty_websocket_head_126 hdr = {0};
		hdr.payload_length = length;
		memcpy(hdr.mask_key, mask, 4);

		memcpy(buffer, &head, sizeof(nty_ophdr));
		memcpy(buffer+sizeof(nty_ophdr), &hdr, sizeof(nty_websocket_head_126));
		size = sizeof(nty_websocket_head_126);
		
	} else {
		
		nty_websocket_head_127 hdr = {0};
		hdr.payload_length = length;
		memcpy(hdr.mask_key, mask, 4);
		
		memcpy(buffer, &head, sizeof(nty_ophdr));
		memcpy(buffer+sizeof(nty_ophdr), &hdr, sizeof(nty_websocket_head_127));

		size = sizeof(nty_websocket_head_127);
		
	}

	memcpy(buffer+2, stream, length);

	return length + 2;
}

int transform(struct sockitem *si, struct reactor *mainloop) {

	int ret = 0;
	char mask[4] = {0};
	char *data = decode_packet(si->recvbuf, mask, si->recvlen, &ret);


	printf("data : %s , length : %d\n", data, ret);

	ret = encode_packet(si->sendbuf, mask, data, ret);
	si->sendlen = ret;

	memset(si->recvbuf, 0, BUFLEN);

	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLET;
	//ev.data.fd = clientfd;
	si->fd = si->fd;
	si->callback = send_cb;
	si->status = WB_DATATRANSFROMER;
	ev.data.ptr = si;

	epoll_ctl(mainloop->epfd, EPOLL_CTL_MOD, si->fd, &ev);

	return 0;
}

int send_cb(int fd, int events, char* arg) {
    struct sockitem* it = (struct sockitem*)arg;
    int ret = send(fd, it->sendbuf, it->sendlen, 0);
    if(ret < 0) {
        perror("send");
        return -1;
    }
  
    printf("        srv send -> %s ->%d bite!\n", it->sendbuf, ret);
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.fd = fd;
    it->callback = recv_cb;
    ev.data.ptr = it;

    epoll_ctl(e->epfd, EPOLL_CTL_MOD, fd, &ev);   // 发送就绪
    

    return ret;
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

        epoll_ctl(e->epfd, EPOLL_CTL_DEL, fd, &ev);
        free(si);

        
    }else if(ret == 0) {
        printf("cli %d disconnect!\n", fd);

        close(fd);
        struct epoll_event ev;
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;

        epoll_ctl(e->epfd, EPOLL_CTL_DEL, fd, &ev);
        free(si);
    }else {
        

        if(si->status == WB_HANDSHAKE) {
			printf("request\n");    
			printf("    request info --> %s, %d bite, fd %d\n", si->recvbuf, ret, fd);

			handshark(si, e);
        }else if(si->status == WB_DATATRANSFROMER) {
            transform(si, e);
        }





        
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
    it->status = WB_HANDSHAKE;
    ev.data.ptr = it;

    epoll_ctl(e->epfd, EPOLL_CTL_ADD, clifd, &ev);

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

    e = (struct reactor*)malloc(sizeof(struct reactor));
    e->epfd = epoll_create(EPOLLLEN);

    struct epoll_event ev;

    struct sockitem* item = (struct sockitem*)malloc(sizeof(struct sockitem));
    item->fd = confd;
    item->callback = connect_coute;
    memset(item->recvbuf, 0, BUFLEN);
    memset(item->sendbuf, 0, BUFLEN);
    item->recvlen = BUFLEN;
    item->sendlen = BUFLEN;
    item->status = WB_INIT;
    ev.events = EPOLLIN;
    ev.data.ptr = item;

    epoll_ctl(e->epfd, EPOLL_CTL_ADD, confd, &ev);

    while(1) {

        int nready = epoll_wait(e->epfd, e->events, EPOLLLEN, -1);
        if(nready < 0) {
            return -2; 
        }

        for(int i = 0; i < nready; i++) {
#if 0
            if(e->events[i].events & EPOLLIN) {
                // 连接请求
                struct sockitem* it = (struct sockitem*)e->events[i].data.ptr;
                it->callback(it->fd, e->events[i].events, (char *)it);

            }else if (e->events[i].events & EPOLLOUT) {
                struct sockitem* it = (struct sockitem*)e->events[i].data.ptr;
                it->callback(it->fd, e->events[i].events, (char *)it);
            }
#elif 1

            struct sockitem* it = (struct sockitem*)e->events[i].data.ptr;
            it->callback(it->fd, e->events[i].events, (char *)it);
#endif
        }

    }

}

