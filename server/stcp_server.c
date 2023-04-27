//文件名: server/stcp_server.c
//
//描述: 这个文件包含STCP服务器接口实现. 

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <assert.h>
#include "stcp_server.h"
#include "../topology/topology.h"
#include "../common/helper.h"

//声明tcbtable为全局变量
server_tcb_t *TCB[MAX_TRANSPORT_CONNECTIONS];
//声明到SIP进程的连接为全局变量
int sip_conn;

/*********************************************************************/
//
//STCP API实现
//
/*********************************************************************/

// 这个函数初始化TCB表, 将所有条目标记为NULL. 它还针对TCP套接字描述符conn初始化一个STCP层的全局变量, 
// 该变量作为sip_sendseg和sip_recvseg的输入参数. 最后, 这个函数启动seghandler线程来处理进入的STCP段.
// 服务器只有一个seghandler.
void stcp_server_init(int conn) {
    sip_conn = conn;
    bzero(TCB, sizeof(TCB));
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, seghandler, NULL);
}

// 这个函数查找服务器TCB表以找到第一个NULL条目, 然后使用malloc()为该条目创建一个新的TCB条目.
// 该TCB中的所有字段都被初始化, 例如, TCB state被设置为CLOSED, 服务器端口被设置为函数调用参数server_port. 
// TCB表中条目的索引应作为服务器的新套接字ID被这个函数返回, 它用于标识服务器端的连接. 
// 如果TCB表中没有条目可用, 这个函数返回-1.
int stcp_server_sock(unsigned int server_port) {
    int i_sock = 0;
    for (; i_sock < MAX_TRANSPORT_CONNECTIONS && TCB[i_sock]; ++i_sock);
    assert(i_sock <= MAX_TRANSPORT_CONNECTIONS);
    if (i_sock == MAX_TRANSPORT_CONNECTIONS)return -1;

    TCB[i_sock] = new(server_tcb_t);
    server_tcb_t *entry = TCB[i_sock];
    // initialize server node ID & port
    entry->server_portNum = server_port;
    entry->server_nodeID = topology_getMyNodeID();
    // initialize client node ID & port
    entry->client_portNum = 0;
    entry->client_nodeID = 0;
    // stcp is not ready before stcp_server_accept
    entry->bufMutex = new(pthread_mutex_t);
    pthread_mutex_init(entry->bufMutex, NULL);
    entry->state = CLOSED;
    entry->expect_seqNum = 0;
    entry->recvBuf = (char *) malloc(RECEIVE_BUF_SIZE);
    entry->usedBufLen = 0;
    return i_sock;
}

// 这个函数使用sockfd获得TCB指针, 并将连接的state转换为LISTENING. 它然后启动定时器进入忙等待直到TCB状态转换为CONNECTED 
// (当收到SYN时, seghandler会进行状态的转换). 该函数在一个无穷循环中等待TCB的state转换为CONNECTED,  
// 当发生了转换时, 该函数返回1. 你可以使用不同的方法来实现这种阻塞等待.
int stcp_server_accept(int sockfd) {
    server_tcb_t *entry = TCB[sockfd];
    if (entry == NULL) {
        perror("[Server] accept: client socket invalid\n");
        return -2;
    } else if (entry->state != CLOSED) {
        perror("[Server] accept: connection is not closed\n");
        return -3;
    }
    printf("[Server] listen on socket %d\n", sockfd);
    entry->state = LISTENING;
    while (entry->state == LISTENING) {
        usleep(nstoms(ACCEPT_POLLING_INTERVAL));
    }
    if (entry->state == CLOSED) {
        // son connection is closed;
        return -1;
    }
    return 1;
}

// 接收来自STCP客户端的数据. 这个函数每隔RECVBUF_POLLING_INTERVAL时间
// 就查询接收缓冲区, 直到等待的数据到达, 它然后存储数据并返回1. 如果这个函数失败, 则返回-1.
int stcp_server_recv(int sockfd, void *buf, unsigned int length) {
    server_tcb_t *tcb = TCB[sockfd];
    if (tcb == NULL || tcb->state != CONNECTED) {
        printf("[Server] missing socket or socket is not connected\n");
        return -1;
    }
    long int start_nano = now_nano();
    while (tcb->usedBufLen < length) {
        sleep(RECVBUF_POLLING_INTERVAL);
        if (tcb->state != CONNECTED) return -1;
    }
    // data is ready
    pthread_mutex_lock(tcb->bufMutex);
    memcpy(buf, tcb->recvBuf, length);
    tcb->usedBufLen -= length;
    memmove(tcb->recvBuf, tcb->recvBuf + length, tcb->usedBufLen);
    pthread_mutex_unlock(tcb->bufMutex);
    printf("[Server] receive is done, costs %f s\n", (float) nstos(now_nano() - start_nano));
    return 0;
}

// 这个函数调用free()释放TCB条目. 它将该条目标记为NULL, 成功时(即位于正确的状态)返回1,
// 失败时(即位于错误的状态)返回-1.
int stcp_server_close(int sockfd) {
    if (TCB[sockfd] == NULL) return 1;
    if (TCB[sockfd] && (TCB[sockfd]->state == CLOSED || TCB[sockfd]->state == CLOSEWAIT)) {
        pthread_mutex_destroy(TCB[sockfd]->bufMutex);
        free(TCB[sockfd]->bufMutex);
        free(TCB[sockfd]->recvBuf);
        free(TCB[sockfd]);
        TCB[sockfd] = NULL;
        return 1;
    }
    return -1;
}

// 这是由stcp_server_init()启动的线程. 它处理所有来自客户端的进入数据. seghandler被设计为一个调用sip_recvseg()的无穷循环, 
// 如果sip_recvseg()失败, 则说明到SIP进程的连接已关闭, 线程将终止. 根据STCP段到达时连接所处的状态, 可以采取不同的动作.
// 请查看服务端FSM以了解更多细节.

static inline int get_sip_sock(unsigned int dst_port) {
    int j = 0;
    for (; j < MAX_TRANSPORT_CONNECTIONS; ++j) {
        if (TCB[j] && TCB[j]->server_portNum == dst_port)
            break;
    }
    if (j == MAX_TRANSPORT_CONNECTIONS)return -1;
    return j;
}

void *seghandler(void *arg) {
    int srcNodeID;
    seg_t rcv_seg;
    bzero(&rcv_seg, sizeof(seg_t));
    while (1) {
//        usleep(1);
        int ret = sip_recvseg(sip_conn, &srcNodeID, &rcv_seg);
        if (ret < 0)break;
        if (ret > 0)continue;
        int sock = get_sip_sock(rcv_seg.header.dst_port);
        if (sock < 0)continue;
        server_tcb_t *tcb = TCB[sock];
        if (tcb->state == CLOSED)continue;
        switch (rcv_seg.header.type) {
            case SYN: {
                assert(tcb->state == LISTENING || tcb->state == CONNECTED);
                // SYN is received ready to send SYNACK
                tcb->client_portNum = rcv_seg.header.src_port;
                tcb->client_nodeID = srcNodeID;
                tcb->expect_seqNum = rcv_seg.header.seq_num + 1;
                seg_t *synack = create_seg(tcb->server_portNum, tcb->client_portNum, SYNACK,
                                           0, tcb->expect_seqNum, 0, 0, NULL);
                if (sip_sendseg(sip_conn, (int) tcb->client_nodeID, synack) < 0) exit(1);
                printf("[Server] SYNACK is sent\n");
                tcb->state = CONNECTED;
                tcb->usedBufLen = 0;
                free(synack);
                break;
            }
            case FIN: {
                assert(tcb->state == CONNECTED || tcb->state == CLOSEWAIT);
                seg_t *finack = create_seg(tcb->server_portNum, tcb->client_portNum, FINACK,
                                           0, tcb->expect_seqNum, 0, 0, NULL);
                if (sip_sendseg(sip_conn, (int) tcb->client_nodeID, finack) < 0) exit(1);
                printf("[Server] FINACK for port %u is sent\n", tcb->client_portNum);
                if (tcb->state == CONNECTED) {
                    tcb->state = CLOSEWAIT;
                    tcb->t_close_wait = now_nano();
                }
                free(finack);
                break;
            }
            case DATA: {
                assert(tcb->state == CONNECTED);
                if (tcb->expect_seqNum == rcv_seg.header.seq_num) {
                    if (tcb->usedBufLen + rcv_seg.header.length > RECEIVE_BUF_SIZE) continue;
                    tcb->expect_seqNum += rcv_seg.header.length;
                    pthread_mutex_lock(tcb->bufMutex);
                    memcpy(tcb->recvBuf + tcb->usedBufLen, rcv_seg.data, rcv_seg.header.length);
                    tcb->usedBufLen += rcv_seg.header.length;
                    pthread_mutex_unlock(tcb->bufMutex);
                }
                seg_t *data_ack = create_seg(tcb->server_portNum, tcb->client_portNum, DATAACK,
                                             0, tcb->expect_seqNum, 0, 0, NULL);
                if (sip_sendseg(sip_conn, (int) tcb->client_nodeID, data_ack) < 0)exit(1);
                break;
            }
            default:
                assert(0);
        }
        // should check the first connect waiting in the waiting que to see if it's expired
        long int cur_nano = now_nano();
        for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
            if (TCB[i] && TCB[i]->state == CLOSEWAIT &&
                timeout_nano(cur_nano, TCB[i]->t_close_wait, stons(CLOSEWAIT_TIMEOUT))) {
                TCB[i]->state = CLOSED;
            }
        }
    }

    // son is closed, should clear TCB
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        if (TCB[i]) TCB[i]->state = CLOSED;
    }

    return 0;
}

