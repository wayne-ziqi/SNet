//文件名: son/son.c
//
//描述: 这个文件实现SON进程
//SON进程首先连接到所有邻居, 然后启动listen_to_neighbor线程, 每个该线程持续接收来自一个邻居的进入报文, 并将该报文转发给SIP进程.
//然后SON进程等待来自SIP进程的连接. 在与SIP进程建立连接之后, SON进程持续接收来自SIP进程的sendpkt_arg_t结构, 并将接收到的报文发送到重叠网络中.

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <signal.h>
#include <sys/utsname.h>
#include <assert.h>

#include "../common/constants.h"
#include "../common/pkt.h"
#include "son.h"
#include "../topology/topology.h"
#include "neighbortable.h"

//你应该在这个时间段内启动所有重叠网络节点上的SON进程
#define SON_START_DELAY 10

/**************************************************************/
//声明全局变量
/**************************************************************/

//将邻居表声明为一个全局变量
nbr_tab_t *nt;
//将与SIP进程之间的TCP连接声明为一个全局变量
int sip_conn;

/**************************************************************/
//实现重叠网络函数
/**************************************************************/

// 这个线程打开TCP端口CONNECTION_PORT, 等待节点ID比自己大的所有邻居的进入连接,
// 在所有进入连接都建立后, 这个线程终止.
void *waitNbrs(void *arg) {
    //你需要编写这里的代码.
    struct sockaddr_in cliAddr, servAddr;
    socklen_t clilen = sizeof(cliAddr);
    bzero(&servAddr, sizeof servAddr);
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(CONNECTION_PORT);
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    // get socket and connect
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(socket_fd, (struct sockaddr *) &servAddr, sizeof(servAddr)) == -1) {
        perror("[Son]<waitNbrs> bind tcp socket error\n");
        return NULL;
    }
    if (listen(socket_fd, 1024) == -1) {
        perror("[Son]<waitNbrs> tcp listen error\n");
        return NULL;
    }
    int greaterNum = 0;
    ////
    int myID = topology_getMyNodeID();
    if (myID < 0) return NULL;
    iterate_nbr(nt, nbr) {
        if (nbr->nodeID > myID) ++greaterNum;
    }
    int cnt_greater = 0;
    while (cnt_greater < greaterNum) {
        int cli_fd = accept(socket_fd, (struct sockaddr *) &cliAddr, &clilen);
        if (cli_fd < 0) {
            perror("[Son]<waitNbrs> in tcp socket error");
            return NULL;
        }
        int nodeID = topology_getNodeIDfromip(&cliAddr.sin_addr);
        if (nodeID < myID) {
            close(cli_fd);
            continue;
        }
        int conn = nt_addconn(nt, nodeID, cli_fd);
        if (conn > 0) {
            printf("[Son]<waitNbrs> get a connection: ID: %d, socket %d\n", nodeID, cli_fd);
            ++cnt_greater;
        } else {
            printf("[Son]<waitNbrs> allocate conn error for ID: %d, socket %d\n", nodeID, cli_fd);
            close(cli_fd);
            return 0;
        }
    }
    return 0;
}

// 这个函数连接到节点ID比自己小的所有邻居.
// 在所有外出连接都建立后, 返回1, 否则返回-1.
int connectNbrs(void) {
    //你需要编写这里的代码.
    int myID = topology_getMyNodeID();
    struct sockaddr_in servAddr;
    bzero(&servAddr, sizeof servAddr);
    servAddr.sin_family = AF_INET;
    iterate_nbr(nt, nbr) {
        if (myID > nbr->nodeID) {
            servAddr.sin_port = htons(CONNECTION_PORT);
            servAddr.sin_addr.s_addr = nbr->nodeIP;
            // get socket and connect
            int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
            if (socket_fd == -1) {
                printf("[SON]<connectNbrs> tcp socket error\n");
                return -2;
            }
            if (connect(socket_fd, (struct sockaddr *) &servAddr, sizeof servAddr) < 0) {
                printf("[Son]<connectNbrs> tcp connection failed\n");
                return -3;
            }
            nbr->conn = socket_fd;
        }
    }
    return 1;
}

//每个listen_to_neighbor线程持续接收来自一个邻居的报文. 它将接收到的报文转发给SIP进程.
//所有的listen_to_neighbor线程都是在到邻居的TCP连接全部建立之后启动的.
void *listen_to_neighbor(void *arg) {
    //你需要编写这里的代码.
    nbr_entry_t *nbr = arg;
    sip_pkt_t sipPkt;
    while (1) {
        int rcv = recvpkt(&sipPkt, nbr->conn);
        if (rcv < 0) {
            printf("[Son]<listen_to_neighbor> neighbor offline, node: %d\n", nbr->nodeID);
            // neighbour is offline, kill this thread
            makeNodeFailSipPkt(&sipPkt, nbr->nodeID);
            forwardpktToSIP(&sipPkt, sip_conn);
            wait_nt(nt);
            removeEntry(nt, nbr);
            leave_nt(nt);
            break;
        } else if (rcv == 2) {
            // invalid packet
            continue;
        }
        if (forwardpktToSIP(&sipPkt, sip_conn) < 0) {
            sleep(5);
            continue;
        }
    }
    return 0;
}

//这个函数打开TCP端口SON_PORT, 等待来自本地SIP进程的进入连接.
//在本地SIP进程连接之后, 这个函数持续接收来自SIP进程的sendpkt_arg_t结构, 并将报文发送到重叠网络中的下一跳.
//如果下一跳的节点ID为BROADCAST_NODEID, 报文应发送到所有邻居节点.
void waitSIP(void) {
    //你需要编写这里的代码.
    // NOTE: you should avoid using APIs from topology.h as it will not be updated if some neighbor is offline
    struct sockaddr_in cliAddr, servAddr;
    socklen_t clilen = sizeof(cliAddr);
    bzero(&servAddr, sizeof servAddr);
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(SON_PORT);
    inet_pton(AF_INET, "127.0.0.1", &servAddr.sin_addr);
    // get socket and connect
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(socket_fd, (struct sockaddr *) &servAddr, sizeof(servAddr)) == -1) {
        perror("[Son]<waitSIP> bind tcp socket error\n");
        return;
    }
    if (listen(socket_fd, 1024) == -1) {
        perror("[Son]<waitSIP> tcp listen error\n");
        return;
    }
    sip_conn = accept(socket_fd, (struct sockaddr *) &cliAddr, &clilen);
    if (sip_conn < 0) {
        perror("[Son]<waitNbrs> in tcp socket error");
        return;
    }
    printf("[Son] sip connected\n");
    sip_pkt_t sipPkt;
    int nextNode;
    int needSendFail = 1;
    while (1) {
        if (getpktToSend(&sipPkt, &nextNode, sip_conn) < 0) {
            printf("[Son]<waitSIP> error receive from SIP\n");
            if (needSendFail == 1) {
                printf("[Son]<waitSIP> sending fail packet to neighbors\n");
                makeNodeFailSipPkt(&sipPkt, topology_getMyNodeID());
                wait_nt(nt);
                iterate_nbr(nt, nbr) {
                    if (sendpkt(&sipPkt, nbr->conn) < 0) {
                        printf("[Son]<waitSIP> neighbor offline: Node %d\n", nbr->nodeID);
                    }
                }
                leave_nt(nt);
                needSendFail = 0;
            }
            sip_conn = accept(socket_fd, (struct sockaddr *) &cliAddr, &clilen);
            if (sip_conn > 0) {
                printf("[Son]<waitSIP> sip connected\n");
                needSendFail = 1;
            }
            sleep(5);
            continue;
        }
        if (nextNode == BROADCAST_NODEID) {
            wait_nt(nt);
            iterate_nbr(nt, nbr) {
                if (sendpkt(&sipPkt, nbr->conn) < 0) {
                    printf("[Son]<waitSIP> neighbor offline: Node %d\n", nbr->nodeID);
                }
            }
            leave_nt(nt);
        } else {
            wait_nt(nt);
            nbr_entry_t *nbr = get_nbrEntry_byID(nt, nextNode);
            leave_nt(nt);
            if (!nbr) {
                printf("[Son]<waitSIP> neighbour not found, nodeID: %d\n", nextNode);
            } else if (sendpkt(&sipPkt, nbr->conn) < 0) {
                printf("[Son]<waitSIP> neighbor offline: Node %d\n", nbr->nodeID);
            }
        }
    }// end of while(1)
}

//这个函数停止重叠网络, 当接收到信号SIGINT时, 该函数被调用.
//它关闭所有的连接, 释放所有动态分配的内存.
void son_stop(int type) {
    //你需要编写这里的代码.
    nt_destroy(nt);
    if (sip_conn > 0)close(sip_conn);
    exit(9);
}

int main(void) {
    //启动重叠网络初始化工作
    printf("Overlay network: Node %d initializing...\n", topology_getMyNodeID());

    //创建一个邻居表
    nt = nt_create();
    //将sip_conn初始化为-1, 即还未与SIP进程连接
    sip_conn = -1;

    //注册一个信号句柄, 用于终止进程
    signal(SIGINT, son_stop);

    //打印所有邻居
    int i = 0;
    iterate_nbr(nt, nbr) {
        printf("Overlay network: neighbor %d:%d\n", ++i, nbr->nodeID);
    }

    //启动waitNbrs线程, 等待节点ID比自己大的所有邻居的进入连接
    pthread_t waitNbrs_thread;
    pthread_create(&waitNbrs_thread, NULL, waitNbrs, (void *) 0);

    //等待其他节点启动
    sleep(SON_START_DELAY);

    //连接到节点ID比自己小的所有邻居
    if (connectNbrs() < 0) {
        printf("[Son] error connect to smaller neighbours\n");
    }

    //等待waitNbrs线程返回
    pthread_join(waitNbrs_thread, NULL);

    //此时, 所有与邻居之间的连接都建立好了
    printf("[Son] son connection is ready\n");

    //创建线程监听所有邻居
    iterate_nbr(nt, nbr) {
        pthread_t nbr_listen_thread;
        pthread_create(&nbr_listen_thread, NULL, listen_to_neighbor, (void *) nbr);
    }
    printf("Overlay network: node initialized...\n");
    printf("Overlay network: waiting for connection from SIP process...\n");

    //等待来自SIP进程的连接
    waitSIP();
}
