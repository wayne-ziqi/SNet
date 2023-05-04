//文件名: sip/sip.c
//
//描述: 这个文件实现SIP进程  

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <strings.h>
#include <arpa/inet.h>
#include <signal.h>
#include <netdb.h>
#include <assert.h>
#include <sys/utsname.h>
#include <pthread.h>
#include <unistd.h>

#include "../common/constants.h"
#include "../common/pkt.h"
#include "../common/seg.h"
#include "../topology/topology.h"
#include "sip.h"
#include "routingtable.h"

//SIP层等待这段时间让SIP路由协议建立路由路径. 
#define SIP_WAITTIME 30

/**************************************************************/
//声明全局变量
/**************************************************************/
int son_conn;            //到重叠网络的连接
int stcp_conn;            //到STCP的连接

#define UPDATE_HOP_FLOOR 1020
#define UPDATE_HOP_CEIL 1024

routingtable_t *routingtable;        //路由表
pthread_mutex_t *routingtable_mutex;    //路由表互斥量

#define LOCK_DV pthread_mutex_lock(dv_mutex)
#define UNLOCK_DV pthread_mutex_unlock(dv_mutex)
#define LOCK_ROUTE pthread_mutex_lock(routingtable_mutex)
#define UNLOCK_ROUTE pthread_mutex_unlock(routingtable_mutex)


/**************************************************************/
//实现SIP的函数
/**************************************************************/

//SIP进程使用这个函数连接到本地SON进程的端口SON_PORT.
//成功时返回连接描述符, 否则返回-1.
int connectToSON(void) {
    //你需要编写这里的代码.
    struct sockaddr_in servAddr;
    bzero(&servAddr, sizeof servAddr);
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(SON_PORT);
    inet_pton(AF_INET, "127.0.0.1", &servAddr.sin_addr);
    int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1) {
        printf("[SON]<connectToSON> tcp socket error\n");
        return -1;
    }
    if (connect(sock_fd, (struct sockaddr *) &servAddr, sizeof servAddr) < 0) {
        printf("[Son]<connectToSON> connection failed\n");
        return -3;
    }
    return sock_fd;
}

//这个线程处理来自SON进程的进入报文. 它通过调用son_recvpkt()接收来自SON进程的报文.
//如果报文是SIP报文,并且目的节点就是本节点,就转发报文给STCP进程. 如果目的节点不是本节点,
//就根据路由表转发报文给下一跳.如果报文是路由更新报文,就更新距离矢量表和路由表.
void *pkthandler(void *arg) {
    sip_pkt_t sipPkt;

    while (1) {
        int rcv = son_recvpkt(&sipPkt, son_conn);
        if (rcv < 0) break;
        if (rcv == 2) continue;
        printf("[Sip]<pkthandler> received from son | type: %d, src: %d, dst: %d\n",
               sipPkt.header.type, sipPkt.header.src_nodeID, sipPkt.header.dst_nodeID);
        if (sipPkt.header.type == SIP) {
            if (sipPkt.header.dst_nodeID == topology_getMyNodeID()) {
                // forward to stcp
                if (forwardsegToSTCP(stcp_conn, sipPkt.header.src_nodeID, (seg_t *) sipPkt.data) < 0) {
                    printf("[Sip]<pkthandler:SIP> send to stcp error\n");
                }
            } else {
                // forward to next hop
                LOCK_ROUTE;
                int nextHop = routingtable_getnextnode(routingtable, sipPkt.header.dst_nodeID);
                UNLOCK_ROUTE;
                if (nextHop < 0) {
                    printf("[Sip]<pkthandler:SIP> no route to %d\n", sipPkt.header.dst_nodeID);
                } else {
                    if (son_sendpkt(nextHop, &sipPkt, son_conn) < 0) {
                        printf("[Sip]<pkthandler:SIP> send to next hop error\n");
                    }
                }
            }
        } else if (sipPkt.header.type == ROUTE_UPDATE) {
            // route need updated only when some node is failed, the update packet has only one entry demonstrating the failed host
            pkt_routeupdate_t *pktRouteupdate = (pkt_routeupdate_t *) sipPkt.data;
            int srcNode = sipPkt.header.src_nodeID;
            assert(pktRouteupdate->entryNum == 1);
            for (int i = 0; i < pktRouteupdate->entryNum; ++i) {
                unsigned int dstNode = pktRouteupdate->entry[i].nodeID;
                assert(pktRouteupdate->entry[i].cost == INFINITE_COST);
                LOCK_ROUTE;
                removeNode(routingtable, (int) dstNode);
                UNLOCK_ROUTE;
                // note: we use the same packet to update all nodes, the srcNode ID is used as hop limit
                if (srcNode == dstNode) {   //received from the disconnected node
                    sipPkt.header.src_nodeID = UPDATE_HOP_CEIL;
                    son_sendpkt(BROADCAST_NODEID, &sipPkt, son_conn);
                } else if (srcNode > UPDATE_HOP_FLOOR) {
                    sipPkt.header.src_nodeID = srcNode - 1;
                    son_sendpkt(BROADCAST_NODEID, &sipPkt, son_conn);
                } else {
                    routingtable_print(routingtable);
                }
            }
        } else
            assert(0);
    }
    close(son_conn);
    son_conn = -1;
    pthread_exit(NULL);
}

//这个函数终止SIP进程, 当SIP进程收到信号SIGINT时会调用这个函数. 
//它关闭所有连接, 释放所有动态分配的内存.
void sip_stop(int type) {
    //你需要编写这里的代码.
    if (son_conn > 0)close(son_conn);
    pthread_mutex_destroy(routingtable_mutex);
    free(routingtable_mutex);
    routingtable_destroy(routingtable);
    exit(9);
}

//这个函数打开端口SIP_PORT并等待来自本地STCP进程的TCP连接.
//在连接建立后, 这个函数从STCP进程处持续接收包含段及其目的节点ID的sendseg_arg_t. 
//接收的段被封装进数据报(一个段在一个数据报中), 然后使用son_sendpkt发送该报文到下一跳. 下一跳节点ID提取自路由表.
//当本地STCP进程断开连接时, 这个函数等待下一个STCP进程的连接.
void waitSTCP(void) {
    //你需要编写这里的代码.
    struct sockaddr_in cliAddr, servAddr;
    socklen_t clilen = sizeof(cliAddr);
    bzero(&servAddr, sizeof servAddr);
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(SIP_PORT);
    inet_pton(AF_INET, "127.0.0.1", &servAddr.sin_addr);
    // get socket and connect
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (bind(socket_fd, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0) {
        perror("[Sip]<waitSTCP> bind tcp socket error\n");
    }
    if (listen(socket_fd, 1024) < 0) {
        perror("[Sip]<waitSTCP> tcp listen error\n");
    }
    stcp_conn = accept(socket_fd, (struct sockaddr *) &cliAddr, &clilen);
    if (stcp_conn < 0) {
        perror("[Sip]<waitSTCP> accept tcp error\n");
        return;
    }

    printf("[Sip]<waitSTCP> connected to SIP\n");
    seg_t seg;
    int dstNodeID;
    sip_pkt_t sipPkt;
    while (1) {
        if (getsegToSend(stcp_conn, &dstNodeID, &seg) < 0) {
            printf("[Sip]<waitSTCP> error get packet from STCP\n");
            sleep(5);
            continue;
        }
        printf("[Sip]<waitSTCP> sip get a seg from stcp | type: %s, dstNode: %d\n",
               seg_type_str(seg.header.type), dstNodeID);
        sipPkt.header.src_nodeID = topology_getMyNodeID();
        sipPkt.header.dst_nodeID = dstNodeID;
        sipPkt.header.type = SIP;
        sipPkt.header.length = sizeof(stcp_hdr_t) + seg.header.length;
        memcpy(sipPkt.data, &seg, sipPkt.header.length);
        assert(dstNodeID >= 0);
        LOCK_ROUTE;
        int nextNode = routingtable_getnextnode(routingtable, dstNodeID);
        UNLOCK_ROUTE;
        if (nextNode < 0) {
            printf("[Sip]<waitSTCP> next hop for %d doesn't exist\n", dstNodeID);
        } else {
            printf("[Sip]<waitSTCP> routing to: %d\n", nextNode);
            son_sendpkt(nextNode, &sipPkt, son_conn);
        }
    }
}

int main(int argc, char *argv[]) {
    printf("SIP layer is starting, pls wait...\n");

    //初始化全局变量
    routingtable = routingtable_create();
    routingtable_mutex = (pthread_mutex_t *) malloc(sizeof(pthread_mutex_t));
    pthread_mutex_init(routingtable_mutex, NULL);
    son_conn = -1;
    stcp_conn = -1;

    routingtable_print(routingtable);

    //注册用于终止进程的信号句柄
    signal(SIGINT, sip_stop);

    //连接到本地SON进程
    son_conn = connectToSON();
    if (son_conn < 0) {
        printf("can't connect to SON process\n");
        exit(1);
    }

    //启动线程处理来自SON进程的进入报文
    pthread_t pkt_handler_thread;
    pthread_create(&pkt_handler_thread, NULL, pkthandler, (void *) 0);

    printf("SIP layer is started...\n");
    printf("waiting for routes to be established\n");
    routingtable_print(routingtable);

    //等待来自STCP进程的连接
    printf("waiting for connection from STCP process\n");
    waitSTCP();

}


