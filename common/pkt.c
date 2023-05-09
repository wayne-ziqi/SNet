// 文件名 pkt.c
#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>

#include "pkt.h"

#define SIP_PREFIX "!&"
#define SIP_SUFFIX "!#"
#define PREFIX_LEN 2
#define SUFFIX_LEN 2


#define checkrd(rd_len, noerr, print_error) { \
    if (rd_len <= 0) {               \
        print_error; \
        return noerr;                          \
    }                                       \
}

#define RCV_BEGIN(socket, noerr, print_error) { \
    ssize_t rd;\
    enum {\
        SEGSTART1, SEGSTART2, SEGRECV, SEGSTOP\
    } state = SEGSTART1;\
    while (1) {\
        char c;\
        rd = recv(socket, &c, 1, 0);\
        checkrd(rd, noerr, print_error);\
        switch (state) {\
            case SEGSTART1: {\
                if (c == SIP_PREFIX[0])state = SEGSTART2;\
                break;\
            }\
            case SEGSTART2: {\
                if (c == SIP_PREFIX[1])state = SEGRECV;\
                else state = SEGSTART1;\
                break;\
            }\
            default:\
                assert(0);\
        }\
        if (state == SEGRECV)break;\
    }                                                \
}

#define RCV_END(socket, noerr, print_error) { \
    char suf[SUFFIX_LEN + 1];\
    bzero(suf, sizeof(suf));\
    rd = recv(socket, suf, SUFFIX_LEN, 0);\
    checkrd(rd, noerr, print_error);\
    if (strcmp(suf, SIP_SUFFIX) != 0) {\
        printf("[WARN] the packet is invalid\n");\
        return noerr;\
    }                                             \
}

// make connection fail packet
void makeNodeFailSipPkt(sip_pkt_t *sipPkt, int loseID) {
    pkt_routeupdate_t nodeLoss;
    nodeLoss.entryNum = 1;
    nodeLoss.entry[0].nodeID = UPDATE_HOP_FLOOR;
    nodeLoss.entry[0].cost = INFINITE_COST;
    sipPkt->header.type = ROUTE_UPDATE;
    sipPkt->header.src_nodeID = loseID;
    sipPkt->header.dst_nodeID = BROADCAST_NODEID;
    sipPkt->header.length = (unsigned short) (sizeof(nodeLoss.entryNum) +
                                              nodeLoss.entryNum * sizeof(routeupdate_entry_t));
    memcpy(sipPkt->data, &nodeLoss, sipPkt->header.length);
}


// son_sendpkt()由SIP进程调用, 其作用是要求SON进程将报文发送到重叠网络中. SON进程和SIP进程通过一个本地TCP连接互连.
// 在son_sendpkt()中, 报文及其下一跳的节点ID被封装进数据结构sendpkt_arg_t, 并通过TCP连接发送给SON进程. 
// 参数son_conn是SIP进程和SON进程之间的TCP连接套接字描述符.
// 当通过SIP进程和SON进程之间的TCP连接发送数据结构sendpkt_arg_t时, 使用'!&'和'!#'作为分隔符, 按照'!& sendpkt_arg_t结构 !#'的顺序发送.
// 如果发送成功, 返回1, 否则返回-1.
int son_sendpkt(int nextNodeID, sip_pkt_t *pkt, int son_conn) {
    if (send(son_conn, SIP_PREFIX, PREFIX_LEN, 0) < 0) {
        printf("[Sip]<son_sendpkt> send prefix to SON error\n");
        return -1;
    }
    if (send(son_conn, &nextNodeID, sizeof(int), 0) < 0) {
        printf("[Sip]<son_sendpkt> send nextNodeID to SON error\n");
        return -1;
    }
    if (send(son_conn, pkt, sizeof(sip_hdr_t) + pkt->header.length, 0) < 0) {
        printf("[Sip]<son_sendpkt> send packet to SON error\n");
        return -1;
    }
    if (send(son_conn, SIP_SUFFIX, SUFFIX_LEN, 0) < 0) {
        printf("[Sip]<son_sendpkt> send suffix to SON error\n");
        return -1;
    }
    return 1;
}

// son_recvpkt()函数由SIP进程调用, 其作用是接收来自SON进程的报文. 
// 参数son_conn是SIP进程和SON进程之间TCP连接的套接字描述符. 报文通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#. 
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点 
// PKTSTART2 -- 接收到'!', 期待'&' 
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收 
// 如果成功接收报文, 返回1, 否则返回-1.
int son_recvpkt(sip_pkt_t *pkt, int son_conn) {
    RCV_BEGIN(son_conn, -1, printf("[Sip]<son_recvpkt> receive prefix error\n"))
    ssize_t rd = recv(son_conn, &pkt->header, sizeof(sip_hdr_t), 0);
    checkrd(rd, -1, printf("[Sip]<son_recvpkt> receive header error\n"))
    int data_len = pkt->header.length;
    if (data_len > 0) {
        rd = recv(son_conn, &pkt->data, data_len, 0);
        checkrd(rd, -1, printf("[Sip]<son_recvpkt> receive data error\n"))
    }
    RCV_END(son_conn, 2, printf("[Sip]<son_recvpkt> receive suffix error\n"))
    return 1;
}

// 这个函数由SON进程调用, 其作用是接收数据结构sendpkt_arg_t.
// 报文和下一跳的节点ID被封装进sendpkt_arg_t结构.
// 参数sip_conn是在SIP进程和SON进程之间的TCP连接的套接字描述符. 
// sendpkt_arg_t结构通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#. 
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点 
// PKTSTART2 -- 接收到'!', 期待'&' 
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收
// 如果成功接收sendpkt_arg_t结构, 返回1, 否则返回-1.
int getpktToSend(sip_pkt_t *pkt, int *nextNode, int sip_conn) {
    RCV_BEGIN(sip_conn, -1, printf("[Son]<getpktToSend> can't receive prefix\n"))
    ssize_t rd = recv(sip_conn, nextNode, sizeof(int), 0);
    checkrd(rd, -1, printf("[Son]<getpktToSend> can't receive nextNode\n"))
    rd = recv(sip_conn, &pkt->header, sizeof(sip_hdr_t), 0);
    checkrd(rd, -1, printf("[Son]<getpktToSend> can't receive packet header\n"))
    int data_len = pkt->header.length;
    if (data_len != 0) {
        rd = recv(sip_conn, &pkt->data, data_len, 0);
        checkrd(rd, -1, printf("[Son]<getpktToSend> can't receive sip data\n"))
    }
    RCV_END(sip_conn, -1, printf("[Son]<getpktToSend> can't receive sip suffix\n"))
    return 1;
}

// forwardpktToSIP()函数是在SON进程接收到来自重叠网络中其邻居的报文后被调用的. 
// SON进程调用这个函数将报文转发给SIP进程. 
// 参数sip_conn是SIP进程和SON进程之间的TCP连接的套接字描述符. 
// 报文通过SIP进程和SON进程之间的TCP连接发送, 使用分隔符!&和!#, 按照'!& 报文 !#'的顺序发送. 
// 如果报文发送成功, 返回1, 否则返回-1.
int forwardpktToSIP(sip_pkt_t *pkt, int sip_conn) {
    if (send(sip_conn, SIP_PREFIX, PREFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[Son]<forwardpktToSIP> send prefix to SIP error\n");
        return -1;
    }
    if (send(sip_conn, pkt, sizeof(sip_hdr_t) + pkt->header.length, MSG_NOSIGNAL) < 0) {
        printf("[Son]<forwardpktToSIP> send packet to SIP error\n");
        return -1;
    }
    if (send(sip_conn, SIP_SUFFIX, SUFFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[Son]<forwardpktToSIP> send suffix to SIP error\n");
        return -1;
    }
    return 1;
}

// sendpkt()函数由SON进程调用, 其作用是将接收自SIP进程的报文发送给下一跳.
// 参数conn是到下一跳节点的TCP连接的套接字描述符.
// 报文通过SON进程和其邻居节点之间的TCP连接发送, 使用分隔符!&和!#, 按照'!& 报文 !#'的顺序发送. 
// 如果报文发送成功, 返回1, 否则返回-1.
int sendpkt(sip_pkt_t *pkt, int conn) {
    if (send(conn, SIP_PREFIX, PREFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[Son]<sendpkt> send packet to neighbor error, connection %d\n", conn);
        return -1;
    }
    if (send(conn, pkt, sizeof(sip_hdr_t) + pkt->header.length, MSG_NOSIGNAL) < 0) {
        printf("[Son]<sendpkt> send packet to neighbor error, connection %d\n", conn);
        return -1;
    }
    if (send(conn, SIP_SUFFIX, SUFFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[Son]<sendpkt> send packet to neighbor error, connection %d\n", conn);
        return -1;
    }
    return 1;
}

// recvpkt()函数由SON进程调用, 其作用是接收来自重叠网络中其邻居的报文.
// 参数conn是到其邻居的TCP连接的套接字描述符.
// 报文通过SON进程和其邻居之间的TCP连接发送, 使用分隔符!&和!#. 
// 为了接收报文, 这个函数使用一个简单的有限状态机FSM
// PKTSTART1 -- 起点 
// PKTSTART2 -- 接收到'!', 期待'&' 
// PKTRECV -- 接收到'&', 开始接收数据
// PKTSTOP1 -- 接收到'!', 期待'#'以结束数据的接收 
// 如果成功接收报文, 返回1, 否则返回-1.
int recvpkt(sip_pkt_t *pkt, int conn) {
    RCV_BEGIN(conn, -1, printf("[Son]<recvpkt> can't receive prefix\n"))
    ssize_t rd = recv(conn, &pkt->header, sizeof(sip_hdr_t), 0);
    checkrd(rd, -1, printf("[Son]<recvpkt> can't receive header\n"))
    int data_len = pkt->header.length;
    if (data_len > 0) {
        rd = recv(conn, &pkt->data, data_len, 0);
        checkrd(rd, -1, printf("[Son]<recvpkt> can't receive data\n"))
    }
    RCV_END(conn, 2, printf("[Son]<recvpkt> can't receive suffix\n"))
    return 1;
}
