#include <sys/epoll.h>
#include <string.h>
#include <zconf.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#define BUFFER_LENGTH   4096
#define MAX_EVENTS      1024
#define SERVER_PORT     10086


typedef int NCALLBACK(int fd, int events, void *arg);
int recv_cb(int fd, int events, void *arg);
int send_cb(int fd, int events, void *arg);

struct ntyevent{
    int fd;         //socket
    int events;      //sockfd: read / write
    void *arg;
    NCALLBACK *callback;  //read / write
    char buffer[BUFFER_LENGTH];
    int length;
    int status;
    long last_active;
};

struct ntyreactor{
    int epfd;
    struct ntyevent *ev;
};

int nty_event_set(struct ntyevent *ev, int fd, NCALLBACK cb, void *arg){
    ev->fd = fd;
    ev->callback = cb;
    ev->events = 0;
    ev->arg = arg;
    ev->last_active = time(NULL);
}

//将ntyevent添加到epoll中
//add to reactor
int nty_event_add(int epfd, int events, struct ntyevent *ev){
    struct epoll_event ep_ev = {0};
    ep_ev.data.ptr = ev;
    ep_ev.events = ev->events = events;

    int oper;
    if (ev->status == 1) {  //在epoll中，防止重复添加
        oper = EPOLL_CTL_MOD;
    } else {
        oper = EPOLL_CTL_ADD;
        ev->status = 1;
    }

    if (epoll_ctl(epfd, oper, ev->fd, &ep_ev) < 0){
        printf("event add failed %d\n", ev->fd);
        return -1;
    }
    return 0;
}

//delete from reactor
int nty_event_del(int epfd, struct ntyevent *ev){
    struct epoll_event ep_ev = {0};
    if (ev->status != 1) {
        return -1;
    }

    ep_ev.data.ptr = ev;
    ev->status = 0;

    if (epoll_ctl(epfd, EPOLL_CTL_DEL, ev->fd, &ep_ev) < 0){
        printf("event del failed &d\n", ev->fd);
        return -1;
    }

    return 0;
}

int ntyreactor_init(struct ntyreactor *reactor){
    if (reactor == NULL) return -1;
    memset(reactor, 0, sizeof(struct ntyreactor));

    reactor->epfd = epoll_create(1);
    if (reactor->epfd < 0){
        printf("cerate epfd err : %s\n", strerror(errno));
        return -2;
    }
    reactor->ev = (struct ntyevent*)malloc(MAX_EVENTS * sizeof(struct ntyevent));
    if (reactor->ev == NULL){
        printf("malloc failed err : %s\n", strerror(errno));
        close(reactor->epfd);
        return -3;
    }
}

int ntyreactor_destory(struct ntyreactor *reactor){
    if (reactor == NULL) return -1;

    close(reactor->epfd);
    free(reactor->ev);

    return 0;
}

int init_sock(short port){
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, 0, O_NONBLOCK);

    struct sockaddr_in server_addr = {0};

    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(port);

    bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (listen(fd, 5) < 0){
        printf("listen failed: %s\n", strerror(errno));
        return -1;
    }
    printf("socket 创建成功， fd = %d\n", fd);
    return fd;
}

int send_cb(int fd, int events, void *arg){

    struct ntyreactor *reactor = (struct ntyreactor *) arg;
    struct ntyevent *ev = reactor->ev+fd;

    int len = send(fd, ev->buffer, ev->length, 0);
    if (len > 0) {
        printf("send: %s\n", ev->buffer);

        nty_event_del(reactor->epfd, ev);
        nty_event_set(ev, fd, recv_cb, reactor);
        nty_event_add(reactor->epfd,EPOLLIN, ev);
    } else {
        close(ev->fd);
        nty_event_del(reactor->epfd, ev);
    }
}

int recv_cb(int fd, int events, void *arg){

    struct ntyreactor *reactor = (struct ntyreactor *) arg;
    struct ntyevent *ev = reactor->ev+fd;

    memset(ev->buffer, 0, BUFFER_LENGTH);
    int len = recv(fd, ev->buffer, BUFFER_LENGTH, 0);
    nty_event_del(reactor->epfd, ev);
    if (len > 0){   //recv
        ev->length = len;
        ev->buffer[len] = '\0';

        nty_event_set(ev, fd, send_cb, reactor);
        nty_event_add(reactor->epfd, EPOLLOUT, ev);

    } else if (len == 0){   //disconnect

        close(ev->fd);
        printf("fd=%d, closed\n", fd);

    } else {    //-1

    }

}



int accept_cb(int fd, int events, void *arg){

    struct ntyreactor *reactor = (struct ntyreactor*) arg;
    if (reactor == NULL) return -1;

    struct sockaddr_in client_addr;
    socklen_t len = sizeof(client_addr);

    int clientfd = accept(fd, (struct sockaddr*)&client_addr, &len);
    if (clientfd == -1){
        if (errno != EAGAIN && errno != EINTR){
            printf("accept : %s\n", strerror(errno));
            return -1;
        }
    }

    if (fcntl(clientfd, F_SETFL, O_NONBLOCK) < 0){
        return -2;
    }

    nty_event_set(&reactor->ev[clientfd], clientfd, recv_cb, reactor);
    nty_event_add(reactor->epfd, EPOLLIN, &reactor->ev[clientfd]);

    printf("new connection comming!\n");

}

int ntyreactor_addlistener(struct ntyreactor *reactor, int sockfd, NCALLBACK acceptor){

    if (reactor == NULL) return -1;
    if (reactor->ev == NULL) return -1;

    //把fd以events结构组织起来并放入reactor
    nty_event_set(&reactor->ev[sockfd], sockfd, acceptor, reactor);
    nty_event_add(reactor->epfd, EPOLLIN, &reactor->ev[sockfd]);

    return 0;
}

int ntyreactor_run(struct ntyreactor *reactor){

    if (reactor == NULL) return -1;
    if (reactor->epfd < 0) return -1;
    if (reactor->ev == NULL) return -1;

    struct epoll_event events[MAX_EVENTS];

    while (1){


        //check fd timeout --> last active

        int nready = epoll_wait(reactor->epfd, events, MAX_EVENTS, 1000);
        if (nready < 0) continue;

        for (int i = 0; i < nready; i++){
            struct ntyevent *ev = (struct ntyevent *)events[i].data.ptr;

            if ((events[i].events & EPOLLIN) && (ev->events & EPOLLIN)){
                ev->callback(ev->fd, events[i].events, ev->arg);
            }

            if ((events[i].events & EPOLLOUT) && (ev->events & EPOLLOUT)){
                ev->callback(ev->fd, events[i].events, ev->arg);
            }
        }
    }

    return 0;

}