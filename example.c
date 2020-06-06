//
// Created by acmery on 2020/6/6.
//

#include "reactor.h"

int main() {
    int sockfd = init_sock(SERVER_PORT);

    struct ntyreactor *reactor = (struct ntyreactor*)malloc(sizeof(struct ntyreactor));
    ntyreactor_init(reactor);

    ntyreactor_addlistener(reactor, sockfd, accept_cb);

    ntyreactor_run(reactor);

    ntyreactor_destory(reactor);

    close(sockfd);

    return 0;
}