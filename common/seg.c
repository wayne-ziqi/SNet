#include "seg.h"
#include "helper.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <time.h>
#include <assert.h>
#include <string.h>

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
}}

long now_nano(void) {
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    return now.tv_sec * 1000000000 + now.tv_nsec;
}

inline const char *seg_type_str(int type) {
    switch (type) {
        case SYN:
            return "SYN";
        case SYNACK:
            return "SYN_ACK";
        case FIN:
            return "FIN";
        case FINACK:
            return "FIN_ACK";
        case DATA:
            return "DATA";
        case DATAACK:
            return "DATA_ACK";
        default:
            return "UNKNOWN";
    }
}

seg_t *
create_seg(unsigned int src_port, unsigned int dst_port,
           unsigned short type, unsigned int seq_num,
           unsigned int ack_num, unsigned short rcv_win,
           unsigned short length, char *data) {
    seg_t *seg = new(seg_t);
    bzero(seg, sizeof(seg_t));
    seg->header.src_port = src_port;
    seg->header.dst_port = dst_port;
    seg->header.type = type;
    seg->header.seq_num = seq_num;
    seg->header.ack_num = ack_num;
    seg->header.rcv_win = rcv_win;
    assert(length <= MAX_SEG_LEN);
    seg->header.length = length;
    if (length > 0)
        memcpy(seg->data, data, length);
    seg->header.checksum = 0;
    return seg;
}

//STCP进程使用这个函数发送sendseg_arg_t结构(包含段及其目的节点ID)给SIP进程.
//参数sip_conn是在STCP进程和SIP进程之间连接的TCP描述符.
//如果sendseg_arg_t发送成功,就返回1,否则返回-1.
int sip_sendseg(int sip_conn, int dest_nodeID, seg_t *segPtr) {

    assert(segPtr);
    if (send(sip_conn, SIP_PREFIX, PREFIX_LEN, 0) <= 0) {
        printf("[Son] sip_send error\n");
        return -1;
    }
    unsigned short data_len = segPtr->header.length;
    unsigned long valid_seg_len = sizeof(stcp_hdr_t) + data_len;
    segPtr->header.checksum = 0;
    segPtr->header.checksum = checksum(segPtr, (int) valid_seg_len);
    if (send(sip_conn, &dest_nodeID, sizeof(int), 0) <= 0) {
        printf("[Son] sip_send error\n");
        return -1;
    }
    if (send(sip_conn, segPtr, valid_seg_len, 0) <= 0) {
        printf("[Son] sip_send error\n");
        return -1;
    }
    if (send(sip_conn, SIP_SUFFIX, SUFFIX_LEN, 0) <= 0) {
        printf("[Son] sip_send error\n");
        return -1;
    }
    return (int) valid_seg_len;
}

//STCP进程使用这个函数来接收来自SIP进程的包含段及其源节点ID的sendseg_arg_t结构.
//参数sip_conn是STCP进程和SIP进程之间连接的TCP描述符.
//当接收到段时, 使用seglost()来判断该段是否应被丢弃并检查校验和.
//如果成功接收到sendseg_arg_t就返回0, 丢失或checksum错误返回1，否则返回-1.
int sip_recvseg(int sip_conn, int *src_nodeID, seg_t *segPtr) {
    RCV_BEGIN(sip_conn, -1, printf("[SIP]<sip_recvseg> error receive prefix\n"))
    // the prefix is parsed, begin to read header and data
    ssize_t rd = recv(sip_conn, src_nodeID, sizeof(int), 0);
    checkrd(rd, -1, printf("[SIP]<sip_recvseg> error receive src_nodeID\n"))
    rd = recv(sip_conn, &segPtr->header, sizeof(stcp_hdr_t), 0);
    checkrd(rd, -1, printf("[SIP]<sip_recvseg> error receive header\n"))
    unsigned short data_len = segPtr->header.length;
    if (data_len > 0) {
        rd = recv(sip_conn, &segPtr->data, data_len, 0);
        checkrd(rd, -1, printf("[SIP]<sip_recvseg> error receive data\n"))
    }
    RCV_END(sip_conn, -1, printf("[SIP]<sip_recvseg> error receive suffix\n"))
    // simulate busy network
    if (seglost(segPtr) == 1) {
        printf("[Son] \x1B[33mpacket (seq: %u, ack: %u) is dropped\x1B[0m\n", segPtr->header.seq_num,
               segPtr->header.ack_num);
        return 1;
    }
    printf("[SIP]<sip_recvseg> recv a seg | type: %s, length: %d, srcPort: %d, srcNodeID: %d\n",
           seg_type_str(segPtr->header.type), segPtr->header.length,
           segPtr->header.src_port, *src_nodeID
    );
    if (checkchecksum(segPtr, (int) sizeof(stcp_hdr_t) + data_len) < 0) {
        printf("[Son] \x1B[33merror checksum\x1B[0m, packet (seq: %u, ack: %u) is dropped\n",
               segPtr->header.seq_num, segPtr->header.ack_num);
        return 1;
    }
    return 0;
}

//SIP进程使用这个函数接收来自STCP进程的包含段及其目的节点ID的sendseg_arg_t结构.
//参数stcp_conn是在STCP进程和SIP进程之间连接的TCP描述符.
//如果成功接收到sendseg_arg_t就返回1, 否则返回-1.
int getsegToSend(int stcp_conn, int *dest_nodeID, seg_t *segPtr) {
    RCV_BEGIN(stcp_conn, -1, printf("[SIP]<getsegToSend> error receive prefix\n"))
    ssize_t rd = recv(stcp_conn, dest_nodeID, sizeof(int), 0);
    checkrd(rd, -1, printf("[SIP]<getsegToSend> error receive dst_nodeID\n"))
    rd = recv(stcp_conn, &segPtr->header, sizeof(stcp_hdr_t), 0);
    checkrd(rd, -1, printf("[SIP]<getsegToSend> error receive header\n"))
    int data_len = segPtr->header.length;
    if (data_len > 0) {
        rd = recv(stcp_conn, &segPtr->data, data_len, 0);
        checkrd(rd, -1, printf("[SIP]<getsegToSend> error receive data\n"))
    }
    RCV_END(stcp_conn, -1, printf("[SIP]<getsegToSend> error receive suffix\n"))
    return 1;
}

//SIP进程使用这个函数发送包含段及其源节点ID的sendseg_arg_t结构给STCP进程.
//参数stcp_conn是STCP进程和SIP进程之间连接的TCP描述符.
//如果sendseg_arg_t被成功发送就返回1, 否则返回-1.
int forwardsegToSTCP(int stcp_conn, int src_nodeID, seg_t *segPtr) {
    if (send(stcp_conn, SIP_PREFIX, PREFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[SIP]<forwardsegToSTCP> send prefix to STCP error\n");
        return -1;
    }
    if (send(stcp_conn, &src_nodeID, sizeof(int), MSG_NOSIGNAL) < 0) {
        printf("[SIP]<forwardsegToSTCP> send src_nodeID to STCP error\n");
        return -1;
    }
    if (send(stcp_conn, segPtr, sizeof(segPtr->header) + segPtr->header.length, MSG_NOSIGNAL) < 0) {
        printf("[SIP]<forwardsegToSTCP> send packet to STCP error\n");
        return -1;
    }
    if (send(stcp_conn, SIP_SUFFIX, SUFFIX_LEN, MSG_NOSIGNAL) < 0) {
        printf("[SIP]<forwardsegToSTCP> send packet to STCP error\n");
        return -1;
    }
    return 1;
}

// 一个段有PKT_LOST_RATE/2的可能性丢失, 或PKT_LOST_RATE/2的可能性有着错误的校验和.
// 如果数据包丢失了, 就返回1, 否则返回0. 
// 即使段没有丢失, 它也有PKT_LOST_RATE/2的可能性有着错误的校验和.
// 我们在段中反转一个随机比特来创建错误的校验和.
int seglost(seg_t *segPtr) {
    int random = rand() % 100;
    if (random < PKT_LOSS_RATE * 100) {
        //50%可能性丢失段
        if (rand() % 2 == 0) {
            return 1;
        }
            //50%可能性是错误的校验和
        else {
            //获取数据长度
            int len = sizeof(stcp_hdr_t) + segPtr->header.length;
            //获取要反转的随机位
            int errorbit = rand() % (len * 8);
            //反转该比特
            char *temp = (char *) segPtr;
            temp = temp + errorbit / 8;
            *temp = *temp ^ (1 << (errorbit % 8));
            return 0;
        }
    }
    return 0;
}

//这个函数计算指定段的校验和.
//校验和计算覆盖段首部和段数据. 你应该首先将段首部中的校验和字段清零, 
//如果数据长度为奇数, 添加一个全零的字节来计算校验和.
//校验和计算使用1的补码.
unsigned short checksum(seg_t *segment, int size) {
    unsigned short *buf = (unsigned short *) segment;
    unsigned long cksum = 0;
    while (size > 1) {
        cksum += *buf++;
        size -= sizeof(unsigned short);
    }
    if (size)
        cksum += *(unsigned char *) buf;

    cksum = (cksum >> 16) + (cksum & 0xffff);
    cksum += (cksum >> 16);
    return (unsigned short) (~cksum);
}

//这个函数检查段中的校验和, 正确时返回1, 错误时返回-1
int checkchecksum(seg_t *segment, int size) {
    if (checksum(segment, size) == 0)return 1;
    return -1;
}
