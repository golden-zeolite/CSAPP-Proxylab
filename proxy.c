/* 
 * Proxylab: Our proxy listen to request from clients, create one new thread
 * when receiving a request. Then analysis the request, check if the content
 * has been cached, is so, sent data back to client from cache, otherwise 
 * set up connection to sever, pass data to client, store it in cache if size
 * is in range. The cache system use a doubly linked list to keep track of all
 * memory blocks allocated for cache.
 */

#include <stdio.h>
#include <string.h>
#include <semaphore.h>
#include <pthread.h>
#include <stdlib.h>
#include "csapp.h"

#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400

static const char *user_agent = "User-Agent: Mozilla/5.0 (X11; Linux x86_64;\
                                rv:10.0.3) Gecko/20120305 Firefox/10.0.3\r\n";
static const char *accept_header = "Accept: text/html,application/xhtml+xml,\
                                   application/xml;q=0.9,*/*;q=0.8\r\n";
static const char *accept_encoding = "Accept-Encoding: gzip, deflate\r\n";

/* Declare functions */
void *thread(void *vargp);
void proxy(int fd);
int parse_uri(char *uri, char *filename, char *path, int *port);
struct cache_block *search_block(char *uri);
int create_block(char *cache_data, char *uri, int size);
int remove_block(struct cache_block *block);
int open_clientfd_sf(char *hostname, int port);
void sigint_handler(int sig);
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg);

/* Declare global variables and structs */
/* Mutex used for concurrency */
sem_t mutex;
pthread_rwlock_t chain, block;

/* Basic structure of a cache block */
struct cache_block {
    struct cache_block *prev;
    struct cache_block *next;
    char *data;
    char *data_uri;
    int data_size;
}; 

/* Pointer pointing to the first block of block chain */
struct cache_block *cache_ini;  
/* Record of cache size */
int cache_size;

/*
 * main - Open a listen desipher, get into a loop that create a new thread 
 * each time a request is received from client.
 */
int main(int argc, char **argv)
{
    printf("%s%s%s", user_agent, accept_header, accept_encoding);

    /* Check command line args */
    if (argc != 2) {
	fprintf(stderr, "usage: %s <port>\n", argv[0]);
	exit(EXIT_SUCCESS);
    }

    int listenfd, *connfd, port;
    socklen_t clientlen = sizeof(struct sockaddr_in);
    struct sockaddr_in clientaddr;
    pthread_t tid;
    /* Ignore the SIGPIPE signal, don't terminate */
    Signal(SIGPIPE, SIG_IGN);
    Signal(SIGINT, sigint_handler);
    /* Initialize semaphore, global variables and caches */
    sem_init(&mutex, 0, 1);
    pthread_rwlock_init(&chain, NULL);
    pthread_rwlock_init(&block, NULL);
    cache_ini = NULL;
    cache_size = 0;

    port = atoi(argv[1]);
    listenfd = Open_listenfd(port);
    while (1) {
        connfd = malloc(sizeof(int));
	*connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        pthread_create(&tid, NULL, thread, connfd);
    }
    return 0;
}

/*
 * thread routine - Firstly detach thread from main thread, then respond to
 * client request, after that close thread
 */
void *thread(void *vargp) {
    int connfd = *((int *)vargp);
    pthread_detach(pthread_self());
    free(vargp);
    proxy(connfd);
    close(connfd);
    return NULL;
}

/*
 * proxy - Read request line from client, phase it. Then check if target 
 * content has already been cached, if so directly return from cache,
 * otherwise the proxy send request to target server, send the response to
 * client and cache the content.
 */
void proxy(int connfd) {
    printf("\nproxy start\n");
    char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE], 
         hostname[MAXLINE], path[MAXLINE], proxy_cache[MAX_OBJECT_SIZE],
         req2server[MAX_OBJECT_SIZE], rsp2proxy[MAXLINE] ;
    int port = 80, serverfd, rsp_len, i, contain_host = 0, if_cache = 0;
    rio_t rio_client, rio_server;
    struct cache_block *temp;

    memset((void *)buf, '\0', sizeof(buf));
    memset((void *)method, '\0', sizeof(method));
    memset((void *)uri, '\0', sizeof(uri));
    memset((void *)version, '\0', sizeof(version));
    /* Read request line from client */
    Rio_readinitb(&rio_client, connfd);
    if(Rio_readlineb(&rio_client, buf, MAXLINE) < 0) {
        clienterror(connfd, method, "400", "Bad request",
                "Read client HTTP request error");
        printf("Read client HTTP request error");
        return;
    }
    sscanf(buf, "%s %s %s", method, uri, version);
    /* Check the method */
    if (strcasecmp(method, "GET")) { 
       clienterror(connfd, method, "501", "Not Implemented",
                "Proxy does not implement handling of this method");
        return;
    }

    /* Parse uri into hostname, path and port */
    if(parse_uri(uri, hostname, path, &port) < 0) {
        clienterror(connfd, method, "400", "Bad request",
                "Parse uri from client error");
        printf("Parse uri from client error");
        return;
    }

    /* Handle header and make request to server. Here we check if the header
     * from client contains Host, if so, just add it into request as it is,
     * otherwise we add Host header. Then add other headers into request.
     * If header contains other fields, just add it into request as it is. */
    memset((void *)req2server, '\0', sizeof(req2server));
    memset((void *)buf, '\0', sizeof(buf));
    sprintf(req2server, "%s %s HTTP/1.0\r\n", method, uri);

    if(Rio_readlineb(&rio_client, buf, MAXLINE) <= 0) {
        clienterror(connfd, method, "400", "Bad request",
            "Read client HTTP request header error");
        printf("Read client HTTP request header error");        
        return;
    }

    while(strcmp(buf, "\r\n")) {
        if(strstr(buf, "Host: ") != NULL) {
            strcat(req2server, buf);
            contain_host = 1;
        } else if(!((strstr(buf, "User-Agent: ") != NULL)||
                    (strstr(buf, "Accept: ") != NULL)||
                    (strstr(buf, "Accept-Encoding: ") != NULL)||
                    (strstr(buf, "Connection: ") != NULL)||
                    (strstr(buf, "Proxy-Connection: ") != NULL)))
            strcat(req2server, buf);
        memset((void *)buf, '\0', sizeof(buf));
        if(Rio_readlineb(&rio_client, buf, MAXLINE) <= 0) {
            clienterror(connfd, method, "400", "Bad request",
                "Read client HTTP request header error");
            printf("Read client HTTP request header error");
            return;
        }
    }

    if(!contain_host) {
        strcat(req2server, "Host: ");
        strcat(req2server, hostname);
        strcat(req2server, "\r\n");
    }
    strcat(req2server, user_agent);
    strcat(req2server, accept_header);
    strcat(req2server, accept_encoding);
    strcat(req2server, "Connection: close\r\n");
    strcat(req2server, "Proxy-Connection: close\r\n\r\n");
    
    /* Check if the content has already been cached */
    pthread_rwlock_wrlock(&block);
    pthread_rwlock_rdlock(&chain);
    temp = search_block(uri);
    pthread_rwlock_unlock(&chain);

    if(temp != NULL) {
        printf("data sent from cache\n");
        /* Send data to client from cache */
        if(rio_writen(connfd, temp->data, temp->data_size) !=
           temp->data_size) {
            clienterror(connfd, method, "400", "Bad request",
                "Proxy has a problem while sending respond to client");
            printf("Proxy has a problem while sending respond to client");
            pthread_rwlock_unlock(&block);
            return;
        }
        pthread_rwlock_unlock(&block);
        /* Move this cache block to the front of block chain */
        pthread_rwlock_wrlock(&chain);
        if(temp->prev != NULL) {
            if(temp->next != NULL)
                temp->next->prev = temp->prev;
            temp->prev->next = temp->next;
            temp->prev = NULL;
            temp->next = cache_ini;
            cache_ini->prev = temp;
            cache_ini = temp;
        }
        pthread_rwlock_unlock(&chain);
        return;     
    }
    pthread_rwlock_unlock(&block);

    /* Not cached, set up connection to server and send HTTP request */
    printf("data sent from server\n");
    if((serverfd = open_clientfd_sf(hostname, port)) < 0) {
        clienterror(connfd, method, "400", "Bad request",
                "Proxy has a problem while connecting to server");
        printf("Proxy has a problem while connecting to server");
        close(serverfd);
        return;
    }

    if(rio_writen(serverfd, req2server, strlen(req2server)) != 
                  strlen(req2server)) {
        clienterror(connfd, method, "400", "Bad request",
                "Proxy has a problem while sending request to server");
        printf("Proxy has a problem while sending request to server");
        close(serverfd);
        return;
    }
    /* Get response from server */
    rsp_len = 0, i = 0;
    memset((void *)proxy_cache, '\0', sizeof(proxy_cache));
    memset((void *)rsp2proxy, '\0', sizeof(rsp2proxy));
    Rio_readinitb(&rio_server, serverfd);
    while((i = Rio_readnb(&rio_server, rsp2proxy, MAXLINE)) > 0) {
        if(rio_writen(connfd, rsp2proxy, i) != i) {
            clienterror(connfd, method, "400", "Bad request",
                "Proxy has a problem while sending respond to client");
            printf("Proxy has a problem while sending respond to client");
            close(serverfd);
            return;
        }
        if(!if_cache) {
            if((rsp_len+i) <= MAX_OBJECT_SIZE) {
                memcpy(proxy_cache+rsp_len, rsp2proxy, i);
                rsp_len += i;
            } else
                if_cache = 1;
        }
        memset((void *)rsp2proxy, '\0', sizeof(rsp2proxy));
    }

    /* Decide if it's needed to cache the content, if so, create a block and
       insert into block chain */

    if(!if_cache) {
        /* Check if the size of cache reach the maxium, if so, delete the
           blcoks in the end of chain until space is enough */
        pthread_rwlock_wrlock(&block);
        pthread_rwlock_wrlock(&chain);
        while((cache_size+rsp_len) > MAX_CACHE_SIZE) {
            temp = cache_ini;
            while(temp->next != NULL)
                temp = temp->next;
            i = remove_block(temp);
            cache_size -= i;
        }
        if(create_block(proxy_cache, uri, rsp_len) < 0) {
            printf("Proxy has problem while creating cache block for content");
            close(serverfd);
            pthread_rwlock_unlock(&chain);
            pthread_rwlock_unlock(&block);
            return;
        }
        cache_size += rsp_len;
        pthread_rwlock_unlock(&chain);
        pthread_rwlock_unlock(&block);
    }
    close(serverfd);
    return;
}

/*
 * parse_uri - Parse URI into hostname, file path and port number
 * Return -1 when error occur, else return 0
 */
int parse_uri(char *uri, char *hostname, char* path, int *port) 
{
    int i;
    char buf_uri[MAXLINE], *temp;
    memset((void *)buf_uri, '\0', sizeof(buf_uri));
    strcpy(buf_uri, uri);

    memset((void *)hostname, '\0', sizeof(hostname));
    memset((void *)path, '\0', sizeof(path));
    if((temp = strstr(buf_uri, "http://")) == NULL) {
        return -1;
    }
    else 
        temp += strlen("http://");

    for(i = 0; ;i++) {
        if((temp[i] == ':')||(temp[i] == '/'))
            break;
        else
            hostname[i] = temp[i];
    }
    hostname[i] = '\0';
    if(temp[i] == ':')
        sscanf(&temp[i+1], "%d%s", port, path);
    else
        sscanf(&temp[i], "%s", path);

    printf("Uri finish, hostname %s path %s\r\n", hostname, path);
    return 0;
}


/*
 * search_block - Given the uri of HTTP request from client, search the
 * cache block chain to determine if corresponding data has already been
 * cached. If so, return the pointer of block, otherwise return NULL
 */
struct cache_block *search_block(char *uri) {
    printf("Start to search block\n");
    struct cache_block *temp = cache_ini;
    while(temp != NULL) {
        if(!strcmp(temp->data_uri, uri))
            return temp;
        else
            temp = temp->next;
    }
    return NULL;
}

/*
 * create_block - Given data that we want to cache, create a cache block and 
 * store related information into it. Then put this block as the first one
 * of block chain. Return -1 if problem occurs, otherwise return 0
 */
int create_block(char *cache_data, char *uri, int size) {
    /* Allocate memory space for a cache block */
    struct cache_block *temp = (struct cache_block *)malloc(sizeof
                               (struct cache_block));
    /* Allocate memory space to store data and corresponding uri, then copy 
       data and uri to space allocated */
    temp->data = (char *)malloc(size);
    temp->data_uri = (char *)malloc(MAXLINE);
    memset((void *)temp, '\0', sizeof(temp));
    memset((void *)temp->data, '\0', sizeof(temp->data));
    memset((void *)temp->data_uri, '\0', sizeof(temp->data_uri));

    if(memcpy(temp->data, cache_data, size) != temp->data) {
        free(temp->data);
        free(temp->data_uri);
        free(temp);
        return -1;
    }
    if(strcpy(temp->data_uri, uri) != temp->data_uri) {
        free(temp->data);
        free(temp->data_uri);
        free(temp);
        return -1;
    }
    /* Record the size of data */
    temp->data_size = size;
    /* Set the prev and next pointer, put this block in the first place of
       blocks chain */
    if(cache_ini == NULL) {
        /* There is no block in the chain */
        temp->prev = NULL;
        temp->next = NULL;
        cache_ini = temp;
    } else {
        /* There is at least one block in the chain */
        temp->prev = NULL;
        temp->next = cache_ini;
        cache_ini->prev = temp;
        cache_ini = temp;
    }
    return 0;
}

/*
 * remove_block - Remove target block from the block chain, return the size
 * of data if successful
 */
int remove_block(struct cache_block *block) {
    /* Record the size of data for return, then free the space for storing
       data and uri */
    int return_size = block->data_size;
    /* Check if this is the first block in the chain */
    if(block->prev == NULL) {
        /* Check if this is the only block in the chain */
        if(block->next == NULL)
            cache_ini = NULL;
        else {
            block->next->prev = NULL;
            cache_ini = block->next;
        }
    } else {
        /* Check if this is the last one */
        if(block->next == NULL)
            block->prev->next = NULL;
        else {
            block->prev->next = block->next;
            block->next->prev = block->prev;
        }
    }
    /* Free this cache block, return data size */
    free(block->data);
    free(block->data_uri);
    free(block);
    return return_size;
}

/*  
 * open_listenfd_sf - Safe version to open and return a listening socket 
 * on port, use P and V function to ensure safety. 
 * Returns -1 and sets errno on Unix error.
 */
int open_clientfd_sf(char *hostname, int port) {
    int clientfd;
    struct hostent *hp;
    struct sockaddr_in serveraddr;

    if ((clientfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	return -1; /* check errno for cause of error */

    P(&mutex);
    /* Fill in the server's IP address and port */
    if ((hp = gethostbyname(hostname)) == NULL)
	return -2; /* check h_errno for cause of error */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)hp->h_addr_list[0], 
	  (char *)&serveraddr.sin_addr.s_addr, hp->h_length);
    serveraddr.sin_port = htons(port);
    V(&mutex);

    /* Establish a connection with the server */
    if (connect(clientfd, (SA *) &serveraddr, sizeof(serveraddr)) < 0)
	return -1;
    return clientfd;
}

/* 
 * SIGINT handler -  When receiving SIGINT, free all blocks in the chain and
 * then terminate our proxy
 */
void sigint_handler(int sig)
{
    fprintf(stdout, "\nterminating Web Proxy\n");
    fprintf(stdout, "cleaning cache\n");

    /* Free cache blocks */
    while (cache_ini != NULL)
        remove_block(cache_ini);
    exit(EXIT_SUCCESS);
}

/*
 * clienterror - Returns an error message to the client. Get from tiny proxy.c
 */
void clienterror(int fd, char *cause, char *errnum, 
		 char *shortmsg, char *longmsg) 
{
    char buf[MAXLINE], body[MAXBUF];

    /* Build the HTTP response body */
    sprintf(body, "<html><title>Proxy Error</title>");
    sprintf(body, "%s<body bgcolor=""ffffff"">\r\n", body);
    sprintf(body, "%s%s: %s\r\n", body, errnum, shortmsg);
    sprintf(body, "%s<p>%s: %s\r\n", body, longmsg, cause);
    sprintf(body, "%s<hr><em>The Proxy</em>\r\n", body);

    /* Print the HTTP response */
    sprintf(buf, "HTTP/1.0 %s %s\r\n", errnum, shortmsg);
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-type: text/html\r\n");
    Rio_writen(fd, buf, strlen(buf));
    sprintf(buf, "Content-length: %d\r\n\r\n", (int)strlen(body));
    Rio_writen(fd, buf, strlen(buf));
    Rio_writen(fd, body, strlen(body));
}
