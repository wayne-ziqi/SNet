//文件名: son/neighbortable.c
//
//描述: 这个文件实现用于邻居表的API

#include <stdlib.h>
#include <assert.h>
#include "neighbortable.h"
#include "../topology/topology.h"
#include "../common/helper.h"

#define CONN_INV (-256)
#define NODEID_INV (-256)

topo_graph_t *TopoGraph;

nbr_entry_t *create_entry(int nodeID, in_addr_t nodeIP, int conn) {
    nbr_entry_t *entry = new(nbr_entry_t);
    entry->conn = conn;
    entry->nodeID = nodeID;
    entry->nodeIP = nodeIP;
    entry->next = entry->prev = NULL;
    return entry;
}

void insertEntry(nbr_tab_t *tab, nbr_entry_t *entry) {
    if (!tab || !entry) return;
    nbr_entry_t *next = tab->leader->next;
    nbr_entry_t *pre = tab->leader;
    entry->next = next;
    entry->prev = pre;
    pre->next = entry;
    if (next) next->prev = entry;
    tab->nbr_num += 1;
}

/// get entry by connection or nodeID
nbr_entry_t *getEntry(nbr_tab_t *tab, int conn, int nodeID) {
    if (!tab)return NULL;
    assert(conn == CONN_INV || nodeID == NODEID_INV);
    iterate_nbr(tab, entry) {
        if (entry->conn == conn)return entry;
        if (entry->nodeID == nodeID) return entry;
    }
    return NULL;
}

nbr_entry_t *removeEntry(nbr_tab_t *tab, nbr_entry_t *del) {
    if (!tab || !del) return NULL;
    assert(del != tab->leader);
    nbr_entry_t *pre = del->prev, *next = del->next;
    pre->next = del->next;
    if (next)next->prev = pre;
    free(del);
    tab->nbr_num -= 1;
    return pre;
}


//这个函数首先动态创建一个邻居表. 然后解析文件topology/topology.dat, 填充所有条目中的nodeID和nodeIP字段, 将conn字段初始化为-1.
//返回创建的邻居表.
nbr_tab_t *nt_create(void) {
    nbr_tab_t *tab = new(nbr_tab_t);
    tab->leader = create_entry(0, 0, 0);
    tab->nbr_num = 0;
    pthread_mutex_init(&tab->mtx, NULL);
    topo_node_t *mynode = get_MyNode();
    assert(mynode);
    for (nbr_node_t *nbr = mynode->nbr_list->next; nbr; nbr = nbr->next) {
        insertEntry(tab, create_entry(nbr->neighbor->hostID, nbr->neighbor->hostIP, -1));
    }
    return tab;
}

//这个函数删除一个邻居表. 它关闭所有连接, 释放所有动态分配的内存.
void nt_destroy(nbr_tab_t *nt) {
    wait_nt(nt);
    iterate_nbr(nt, it) {
        if (it->conn > 0) close(it->conn);
    }
    nbr_entry_t *leader = nt->leader;
    while (leader->next) {
        nbr_entry_t *del = leader->next;
        leader->next = del->next;
        free(del);
    }
    leave_nt(nt);
    pthread_mutex_destroy(&nt->mtx);
    free(leader);
    free(nt);
}

nbr_entry_t *get_nbrEntry_byID(nbr_tab_t *nt, int nodeID) {
    return getEntry(nt, CONN_INV, nodeID);
}

//这个函数为邻居表中指定的邻居节点条目分配一个TCP连接. 如果分配成功, 返回1, 否则返回-1.
int nt_addconn(nbr_tab_t *nt, int nodeID, int conn) {
    nbr_entry_t *entry = get_nbrEntry_byID(nt, nodeID);
    if (entry == NULL || entry->conn != -1) return -1;
    entry->conn = conn;
    return 1;
}



