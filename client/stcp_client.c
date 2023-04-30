//文件名: client/stcp_client.c
//
//描述: 这个文件包含STCP客户端接口实现 

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <assert.h>
#include <strings.h>
#include <unistd.h>
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include "../common/helper.h"
#include "../topology/topology.h"
#include "stcp_client.h"
#include "../common/seg.h"

//声明tcbtable为全局变量
client_tcb_t *TCB[MAX_TRANSPORT_CONNECTIONS];
//声明到SIP进程的TCP连接为全局变量
int sip_conn;

//======================================================
//          definition of buffer helpers
//======================================================

/**
 * deep copy from the seg to the send buffer, will help send segment
 */
int add_seg(int sock, seg_t *seg) {
    if (TCB[sock] == NULL || TCB[sock]->state != CONNECTED) {
        printf("[Client] socket missing or state error, add segment to buffer failed");
        return -1;
    }
    client_tcb_t *tcb = TCB[sock];
    segBuf_t *buf = new(segBuf_t);
    memcpy(&buf->seg, seg, sizeof(seg_t));
    buf->next = NULL;
    pthread_mutex_lock(tcb->bufMutex);
    if (tcb->sendBufHead == tcb->sendBufTail) {   // start from an empty buffer
        tcb->sendBufunSent = buf;
        tcb->sendBufTail->next = buf;
        tcb->sendBufTail = buf;
        // create a new thread to traverse the buffer
        printf("[Client] send_buf timer starts\n");
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, sendBuf_timer, tcb);
    } else {
        tcb->sendBufTail->next = buf;
        tcb->sendBufTail = buf;
        if (tcb->sendBufunSent == NULL) tcb->sendBufunSent = buf;
    }
    pthread_mutex_unlock(tcb->bufMutex);
    return 0;
}

// pop segment from the head, atom operation, should be surrounded by lock and unlock
void pop_seg(client_tcb_t *tcb) {
    segBuf_t *first = tcb->sendBufHead->next;
    if (first == tcb->sendBufTail) {
        tcb->sendBufTail = tcb->sendBufHead;
        tcb->sendBufunSent = tcb->sendBufHead;
    }
    tcb->sendBufHead->next = first->next;
    free(first);
}

//======================================================
//          buffer helpers end
//======================================================

/*********************************************************************/
//
//STCP API实现
//
/*********************************************************************/

// 这个函数初始化TCB表, 将所有条目标记为NULL.  
// 它还针对TCP套接字描述符conn初始化一个STCP层的全局变量, 该变量作为sip_sendseg和sip_recvseg的输入参数.
// 最后, 这个函数启动seghandler线程来处理进入的STCP段. 客户端只有一个seghandler.
void stcp_client_init(int conn) {
    sip_conn = conn;
    bzero(TCB, sizeof(TCB));
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, seghandler, NULL);
}

// 这个函数查找客户端TCB表以找到第一个NULL条目, 然后使用malloc()为该条目创建一个新的TCB条目.
// 该TCB中的所有字段都被初始化. 例如, TCB state被设置为CLOSED，客户端端口被设置为函数调用参数client_port. 
// TCB表中条目的索引号应作为客户端的新套接字ID被这个函数返回, 它用于标识客户端的连接. 
// 如果TCB表中没有条目可用, 这个函数返回-1.
int stcp_client_sock(unsigned int client_port) {
    int i_sock = 0;
    for (; i_sock < MAX_TRANSPORT_CONNECTIONS && TCB[i_sock]; ++i_sock);
    assert(i_sock <= MAX_TRANSPORT_CONNECTIONS);
    if (i_sock == MAX_TRANSPORT_CONNECTIONS)return -1;

    TCB[i_sock] = new(client_tcb_t);
    client_tcb_t *entry = TCB[i_sock];
    //  initialize server node ID & port
    entry->server_nodeID = 0;
    entry->server_portNum = 0;
    //  initialize client node ID & port
    entry->client_nodeID = topology_getMyNodeID();
    entry->client_portNum = client_port;
    // stcp is not ready before stcp_client_connect
    entry->state = CLOSED;
    // 0 is the start of SYN
    entry->next_seqNum = 0;
    // send wants to add packets to the tail, and DATA_ACK wants to remove buffers
    entry->bufMutex = new(pthread_mutex_t);
    pthread_mutex_init(entry->bufMutex, NULL);
    // buffer related pointers should all be set to null, lead by a dummy head
    entry->sendBufHead = entry->sendBufTail = entry->sendBufunSent = new(segBuf_t);
    // number of sent-but-not-acked segs
    entry->unAck_segNum = 0;
    return i_sock;
}

// 这个函数用于连接服务器. 它以套接字ID, 服务器节点ID和服务器的端口号作为输入参数. 套接字ID用于找到TCB条目.  
// 这个函数设置TCB的服务器节点ID和服务器端口号,  然后使用sip_sendseg()发送一个SYN段给服务器.  
// 在发送了SYN段之后, 一个定时器被启动. 如果在SYNSEG_TIMEOUT时间之内没有收到SYNACK, SYN 段将被重传. 
// 如果收到了, 就返回1. 否则, 如果重传SYN的次数大于SYN_MAX_RETRY, 就将state转换到CLOSED, 并返回-1.
int stcp_client_connect(int sockfd, int nodeID, unsigned int server_port) {
    client_tcb_t *entry = TCB[sockfd];
    if (entry == NULL) {
        perror("[Client] connect: socket invalid\n");
        return -2;
    } else if (entry->state != CLOSED) {
        perror("[Client] connect: connection is not closed\n");
        return -3;
    }
    entry->server_portNum = server_port;
    entry->server_nodeID = nodeID;
    // make a syn seg
    seg_t *synseg = create_seg(entry->client_portNum, server_port,
                               SYN, entry->next_seqNum, 0, 0, 0, NULL);
    entry->next_seqNum += 1;
    // entry state transfer
    if (sip_sendseg(sip_conn, (int) entry->server_nodeID, synseg) < 0) exit(0);
    entry->state = SYNSENT;
    printf("[Client] SYN 1 is sent\n");
    int retry = 1;
    long int pre_nano = now_nano();
    while (entry->state == SYNSENT && retry < SYN_MAX_RETRY) {
        long int cur_nano = now_nano();
        if (timeout_nano(cur_nano, pre_nano, SYN_TIMEOUT)) {
            pre_nano = cur_nano;
            if (sip_sendseg(sip_conn, (int) entry->server_nodeID, synseg) < 0)exit(0);
            ++retry;
            printf("[Client] time over, retry to send SYN %d\n", retry);

        }
        // necessary: switch the thread to receive
        usleep(1);
    }

    free(synseg);
    if (entry->state == CONNECTED) {
        printf("[Client] connected to server port %d\n", server_port);
        return 1;
    }
    // connection failed
    printf("[Client] tried but fail to connect in %d times\n", SYN_MAX_RETRY);
    assert(retry == SYN_MAX_RETRY);
    entry->state = CLOSED;
    return -1;
}

// 发送数据给STCP服务器. 这个函数使用套接字ID找到TCB表中的条目.
// 然后它使用提供的数据创建segBuf, 将它附加到发送缓冲区链表中.
// 如果发送缓冲区在插入数据之前为空, 一个名为sendbuf_timer的线程就会启动.
// 每隔SENDBUF_ROLLING_INTERVAL时间查询发送缓冲区以检查是否有超时事件发生. 
// 这个函数在成功时返回1，否则返回-1. 
// stcp_client_send是一个非阻塞函数调用.
// 因为用户数据被分片为固定大小的STCP段, 所以一次stcp_client_send调用可能会产生多个segBuf
// 被添加到发送缓冲区链表中. 如果调用成功, 数据就被放入TCB发送缓冲区链表中, 根据滑动窗口的情况,
// 数据可能被传输到网络中, 或在队列中等待传输.
int stcp_client_send(int sockfd, void *data, unsigned int length) {
    if (TCB[sockfd] == NULL || TCB[sockfd]->state != CONNECTED) {
        printf("[Client] send error: tcb missing or not connected\n");
        return -1;
    }
    client_tcb_t *tcb = TCB[sockfd];
    char *buf = data;
    while (length > 0) {
        unsigned short cur_len = length < MAX_SEG_LEN ? length : MAX_SEG_LEN;
        seg_t *seg = create_seg(tcb->client_portNum, tcb->server_portNum, DATA,
                                tcb->next_seqNum, 0, 0, cur_len, buf);
        tcb->next_seqNum += cur_len;
        buf += cur_len, length -= cur_len;
        int err = add_seg(sockfd, seg);
        free(seg);
        if (err) return -1;
    }
    pthread_mutex_lock(tcb->bufMutex);
    while (tcb->sendBufunSent && tcb->unAck_segNum < GBN_WINDOW) {
        tcb->sendBufunSent->sentTime = now_nano();
        if (sip_sendseg(sip_conn, (int) tcb->server_nodeID, &tcb->sendBufunSent->seg) < 0)exit(0);
        ++tcb->unAck_segNum;
        tcb->sendBufunSent = tcb->sendBufunSent->next;
    }
    pthread_mutex_unlock(tcb->bufMutex);
    return 1;
}

// 这个函数用于断开到服务器的连接. 它以套接字ID作为输入参数. 套接字ID用于找到TCB表中的条目.  
// 这个函数发送FIN段给服务器. 在发送FIN之后, state将转换到FINWAIT, 并启动一个定时器.
// 如果在最终超时之前state转换到CLOSED, 则表明FINACK已被成功接收. 否则, 如果在经过FIN_MAX_RETRY次尝试之后,
// state仍然为FINWAIT, state将转换到CLOSED, 并返回-1.
int stcp_client_disconnect(int sockfd) {
    client_tcb_t *tcb = TCB[sockfd];
    if (tcb == NULL) {
        printf("[Client] current socket %u has no connection\n", sockfd);
        return -1;
    }
    if (tcb->state != CONNECTED) {
        perror("[Client] client is not connected\n");
        return -1;
    }
    seg_t *finseg = create_seg(tcb->client_portNum, tcb->server_portNum,
                               FIN, tcb->next_seqNum, 0, 0, 0, NULL);
    tcb->next_seqNum += 1;
    if (sip_sendseg(sip_conn, (int) tcb->server_nodeID, finseg) < 0)return -1;
    tcb->state = FINWAIT;
    printf("[Client] FIN 1 is sent\n");
    int retry = 1;
    long int pre_nano = now_nano();
    while (tcb->state == FINWAIT && retry < FIN_MAX_RETRY) {
        long int cur_nano = now_nano();
        if (timeout_nano(cur_nano, pre_nano, FIN_TIMEOUT)) {
            if (sip_sendseg(sip_conn, (int) tcb->server_nodeID, finseg) < 0)exit(0);
            ++retry;
            printf("[Client] time over, retry to send FIN %d\n", retry);
        }
        // necessary: should switch to receive-thread or FINACK may not be received.
        usleep(1);
    }
    if (tcb->state == FINWAIT) {
        tcb->state = CLOSED;
        printf("[Client] tried but fail to disconnect in %d times\n", FIN_MAX_RETRY);
        return -1;
    }

    printf("[Client] port %u disconnect successfully\n", tcb->client_portNum);
    return 0;
}

// 这个函数调用free()释放TCB条目. 它将该条目标记为NULL, 成功时(即位于正确的状态)返回1,
// 失败时(即位于错误的状态)返回-1.
int stcp_client_close(int sockfd) {
    if (TCB[sockfd] == NULL) return 1;
    if (TCB[sockfd] && TCB[sockfd]->state == CLOSED) {
        pthread_mutex_destroy(TCB[sockfd]->bufMutex);
        free(TCB[sockfd]->bufMutex);
        free(TCB[sockfd]);
        TCB[sockfd] = NULL;
        return 1;
    }
    return -1;
}

// 这是由stcp_client_init()启动的线程. 它处理所有来自服务器的进入段. 
// seghandler被设计为一个调用sip_recvseg()的无穷循环. 如果sip_recvseg()失败, 则说明到SIP进程的连接已关闭,
// 线程将终止. 根据STCP段到达时连接所处的状态, 可以采取不同的动作. 请查看客户端FSM以了解更多细节.

static inline int get_sip_sock(unsigned int src_port, unsigned int dst_port) {
    int j = 0;
    for (; j < MAX_TRANSPORT_CONNECTIONS; ++j) {
        if (TCB[j] && TCB[j]->client_portNum == dst_port && TCB[j]->server_portNum == src_port)
            break;
    }
    if (j == MAX_TRANSPORT_CONNECTIONS)return -1;
    return j;
}

void *seghandler(void *arg) {
    seg_t rcv_seg;
    int srcNodeID;
    bzero(&rcv_seg, sizeof(seg_t));
    while (1) {
        int ret = sip_recvseg(sip_conn, &srcNodeID, &rcv_seg);
        if (ret < 0)break;
        if (ret > 0)continue;
        int sock = get_sip_sock(rcv_seg.header.src_port, rcv_seg.header.dst_port);
        if (sock < 0)continue;
        client_tcb_t *tcb = TCB[sock];
        if (tcb->state == CLOSED) continue;
        switch (rcv_seg.header.type) {
            case SYNACK: {
                if (tcb->state == CONNECTED)continue;
                assert(tcb->state == SYNSENT);
                tcb->state = CONNECTED;
                break;
            }
            case FINACK: {
                if (tcb->state == CLOSED)continue;
                assert(tcb->state == FINWAIT);
                tcb->state = CLOSED;
                break;
            }
            case DATAACK: {
                if (tcb->state != CONNECTED)continue;
                unsigned int ack_num = rcv_seg.header.ack_num;
                pthread_mutex_lock(tcb->bufMutex);
                while (tcb->sendBufHead->next && tcb->sendBufHead->next->seg.header.seq_num < ack_num) {
                    pop_seg(tcb);
                    --tcb->unAck_segNum;
                }
                while (tcb->sendBufunSent && tcb->unAck_segNum < GBN_WINDOW) {
                    tcb->sendBufunSent->sentTime = now_nano();
                    if (sip_sendseg(sip_conn, (int) tcb->server_nodeID, &tcb->sendBufunSent->seg) < 0)exit(0);
                    ++tcb->unAck_segNum;
                    tcb->sendBufunSent = tcb->sendBufunSent->next;
                }
                pthread_mutex_unlock(tcb->bufMutex);
                break;
            }
            default:
                assert(0);
        }
    }
    // son connection is closed, should clear TCB
    for (int i = 0; i < MAX_TRANSPORT_CONNECTIONS; ++i) {
        if (TCB[i]) TCB[i]->state = CLOSED;
    }
    return 0;
}


//这个线程持续轮询发送缓冲区以触发超时事件. 如果发送缓冲区非空, 它应一直运行.
//如果(当前时间 - 第一个已发送但未被确认段的发送时间) > DATA_TIMEOUT, 就发生一次超时事件.
//当超时事件发生时, 重新发送所有已发送但未被确认段. 当发送缓冲区为空时, 这个线程将终止.
void *sendBuf_timer(void *clienttcb) {
    client_tcb_t *tcb = clienttcb;
    while (1) {
        usleep(nstoms(SENDBUF_POLLING_INTERVAL));
        pthread_mutex_lock(tcb->bufMutex);
        // loop condition after '!', connected and not empty
        if (!(tcb->state == CONNECTED && tcb->sendBufHead != tcb->sendBufTail)) {
            pthread_mutex_unlock(tcb->bufMutex);
            break;
        }
        long cur_nano = now_nano();
        if (timeout_nano(cur_nano, tcb->sendBufHead->next->sentTime, DATA_TIMEOUT)) {
            printf("[Client] \x1B[34mdata timeout, begin to resend\x1B[0m\n");
            for (segBuf_t *sb = tcb->sendBufHead->next; sb != tcb->sendBufunSent; sb = sb->next) {
                sb->sentTime = now_nano();
                if (sip_sendseg(sip_conn, (int) tcb->server_nodeID, &sb->seg) < 0)exit(0);
            }
        }
        pthread_mutex_unlock(tcb->bufMutex);
    }
    return 0;
}

