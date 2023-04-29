// 文件名: topology/topology.c
//
// 描述: 这个文件实现一些用于解析拓扑文件的辅助函数
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <assert.h>
#include "topology.h"
#include "../common/helper.h"
#include "../common/constants.h"

topo_graph_t *TopoGraph = NULL;

void init_topo(void);

topo_node_t *get_node_by_id(int id) {
    init_topo();
    for (topo_node_t *node = TopoGraph->leader->next; node; node = node->next) {
        if (node->hostID == id) return node;
    }
    return NULL;
}

topo_node_t *get_MyNode(void) {
    init_topo();
    return TopoGraph->myNode;
}

topo_node_t *get_node_by_name(char *hostname, int create) {
    // !!! should not init_topo here
    for (topo_node_t *node = TopoGraph->leader->next; node; node = node->next) {
        if (strcmp(node->hostname, hostname) == 0) return node;
    }
    if (create == 0) return NULL;
    topo_node_t *new_node = new (topo_node_t);
    strcpy(new_node->hostname, hostname);
    new_node->hostID = topology_getNodeIDfromname(hostname, &new_node->hostIP);
    new_node->nbr_list = new (nbr_node_t);
    new_node->nbr_list->next = NULL;
    new_node->nbr_list->neighbor = NULL;
    new_node->nbr_num = 0;
    new_node->next = TopoGraph->leader->next;
    TopoGraph->leader->next = new_node;
    TopoGraph->node_num += 1;
    return new_node;
}

void add_edge(topo_node_t *host1, topo_node_t *host2, int cost) {
    nbr_node_t *nbr1 = new (nbr_node_t);
    nbr_node_t *nbr2 = new (nbr_node_t);
    nbr1->cost = nbr2->cost = cost;
    nbr1->neighbor = host2, nbr2->neighbor = host1;
    nbr1->next = host1->nbr_list->next;
    host1->nbr_list->next = nbr1;
    host1->nbr_num += 1;
    nbr2->next = host2->nbr_list->next;
    host2->nbr_list->next = nbr2;
    host2->nbr_num += 1;
}

void init_topo(void) {
    if (TopoGraph) return;
    TopoGraph = new (struct topo_graph);
    TopoGraph->node_num = 0;
    TopoGraph->leader = (topo_node_t *) malloc(sizeof(topo_node_t));
    TopoGraph->leader->next = NULL, TopoGraph->leader->nbr_list = NULL;
    char host1[MAX_HOSTNAME_LEN], host2[MAX_HOSTNAME_LEN];
    int cost;
    // FIXME: for convenience, we use absolute path, but should be changed to relative before handing in
    FILE *fp = fopen("../topology/topology.dat", "r");
    if (fp == NULL) {
        perror("[Topo] error opening topology.dat\n");
        return;
    }
    while (fscanf(fp, "%31s %31s %d", host1, host2, &cost) == 3) {
//        printf("[Topo] host1: %s, host2: %s, cost: %d\n", host1, host2, cost);
        topo_node_t *node1 = get_node_by_name(host1, 1);
        topo_node_t *node2 = get_node_by_name(host2, 1);
        if (node1 == NULL || node2 == NULL) continue;
        add_edge(node1, node2, cost);
        bzero(host1, sizeof host1), bzero(host2, sizeof host2);
    }
    fclose(fp);

    char hostName[MAX_HOSTNAME_LEN];
    bzero(hostName, sizeof hostName);
    gethostname(hostName, sizeof hostName);
    TopoGraph->myNode = get_node_by_name(hostName, 0);
    if (TopoGraph->myNode == NULL) {
        printf("[Topo] host name %s doesn't exist in .dat\n", hostName);
    }
}

// 这个函数返回指定主机的节点ID.
// 节点ID是节点IP地址最后8位表示的整数.
// 例如, 一个节点的IP地址为202.119.32.12, 它的节点ID就是12.
// 如果不能获取节点ID, 返回-1.
int topology_getNodeIDfromname(char *hostname, in_addr_t *node_addr) {
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, NULL, &hints, &res);
    if (status != 0) {
        fprintf(stderr, "[Topo] getaddrinfo error: %s\n", gai_strerror(status));
        return -1;
    }
    unsigned id = 65536;
    // currently we only get the first ip address
    for (struct addrinfo *p = res; p; p = p->ai_next) {
        void *addr = NULL;
        char ipstr[INET_ADDRSTRLEN];
        struct sockaddr_in *ipv4 = (struct sockaddr_in *) p->ai_addr;
        addr = &(ipv4->sin_addr);
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        printf("[Topo] host <%s> has address %s\n", hostname, ipstr);
        id = topology_getNodeIDfromip(addr);
        if (node_addr)
            *node_addr = ((struct in_addr *) addr)->s_addr;
        break;
    }
    freeaddrinfo(res); // 释放 getaddrinfo 返回的链表内存
    if (id == 65536) {
        fprintf(stderr, "[Topo] host <%s> has no ip address\n", hostname);
        return -1;
    }
    return (int) id;
}

// 这个函数返回指定的IP地址的节点ID.
// 如果不能获取节点ID, 返回-1.
int topology_getNodeIDfromip(struct in_addr *addr) {
    return (int) ((addr->s_addr >> 24) & 0xff);
}

// 这个函数返回本机的节点ID
// 如果不能获取本机的节点ID, 返回-1.
int topology_getMyNodeID(void) {
    init_topo();
    return TopoGraph->myNode ? TopoGraph->myNode->hostID : -1;
}

const char *getMyName(void) {
    init_topo();
    return TopoGraph->myNode ? TopoGraph->myNode->hostname : NULL;
}

// 这个函数解析保存在文件topology.dat中的拓扑信息.
// 返回邻居数.
int topology_getNbrNum(void) {
    init_topo();
    return TopoGraph->myNode ? TopoGraph->myNode->nbr_num : -1;
}

// 这个函数解析保存在文件topology.dat中的拓扑信息.
// 返回重叠网络中的总节点数.
int topology_getNodeNum(void) {
    init_topo();
    return TopoGraph->node_num;
}

// 这个函数解析保存在文件topology.dat中的拓扑信息.
// 返回一个动态分配的数组, 它包含重叠网络中所有节点的ID.
int *topology_getNodeArray(void) {
    init_topo();
    int *ret = malloc(TopoGraph->node_num * sizeof(int));
    int idx = 0;
    for (topo_node_t *host = TopoGraph->leader->next; host; host = host->next) {
        ret[idx++] = host->hostID;
    }
    return ret;
}

// 这个函数解析保存在文件topology.dat中的拓扑信息.
// 返回一个动态分配的数组, 它包含所有邻居的节点ID.
int *topology_getNbrArray(void) {
    init_topo();
    topo_node_t *host = get_MyNode();
    if (host == NULL) {
        return NULL;
    }
    int *ret = malloc(host->nbr_num * sizeof(int));
    int idx = 0;
    for (nbr_node_t *nbr = host->nbr_list->next; nbr; nbr = nbr->next) {
        ret[idx++] = nbr->neighbor->hostID;
    }
    return ret;
}

// 这个函数解析保存在文件topology.dat中的拓扑信息.
// 返回指定两个节点之间的直接链路代价.
// 如果指定两个节点之间没有直接链路, 返回INFINITE_COST.
unsigned int topology_getCost(int fromNodeID, int toNodeID) {
    if (fromNodeID == toNodeID) return 0;
    init_topo();
    topo_node_t *from = get_node_by_id(fromNodeID);
    for (nbr_node_t *nbr = from->nbr_list->next; nbr; nbr = nbr->next) {
        if (nbr->neighbor->hostID == toNodeID) return nbr->cost;
    }
    return INFINITE_COST;
}
