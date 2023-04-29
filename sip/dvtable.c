
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../common/constants.h"
#include "../common/helper.h"
#include "../topology/topology.h"
#include "dvtable.h"

//这个函数动态创建距离矢量表.
//距离矢量表包含n+1个条目, 其中n是这个节点的邻居数,剩下1个是这个节点本身.
//距离矢量表中的每个条目是一个dv_t结构,它包含一个源节点ID和一个有N个dv_entry_t结构的数组, 其中N是重叠网络中节点总数.
//每个dv_entry_t包含一个目的节点地址和从该源节点到该目的节点的链路代价.
//距离矢量表也在这个函数中初始化.从这个节点到其邻居的链路代价使用提取自topology.dat文件中的直接链路代价初始化.
//其他链路代价被初始化为INFINITE_COST.
//该函数返回动态创建的距离矢量表.

/// create a new dv vector, self is set if its the host itself
static inline dv_entry_t *create_dv_vec(int nodeID) {
    int N = topology_getNodeNum();
    int self = topology_getMyNodeID() == nodeID;
    int *nodeArray = topology_getNodeArray();
    dv_entry_t *vec = new_n(dv_entry_t, N);
    for (int i = 0; i < N; ++i) {
        vec[i].nodeID = nodeArray[i];
        vec[i].cost = self ? topology_getCost(nodeID, vec[i].nodeID) : INFINITE_COST;
    }
    free(nodeArray);
    return vec;
}

dv_tab *dvtable_create(void) {
    // TODO: create dv table
    dv_tab *tab = new(dv_tab);
    tab->dv_num = topology_getNbrNum() + 1;
    tab->dv = new_n(dv_t, tab->dv_num);
    tab->dv[0].nodeID = topology_getMyNodeID();
    tab->dv[0].entry_num = topology_getNodeNum();
    tab->dv[0].dvEntry = create_dv_vec(tab->dv[0].nodeID);
    int *nbrArray = topology_getNbrArray();
    for (int i = 1; i < tab->dv_num; ++i) {
        tab->dv[i].nodeID = nbrArray[i];
        tab->dv[i].entry_num = topology_getNodeNum();
        tab->dv[i].dvEntry = create_dv_vec(nbrArray[i]);
    }
    free(nbrArray);
    return tab;
}

//这个函数删除距离矢量表.
//它释放所有为距离矢量表动态分配的内存.
void dvtable_destroy(dv_tab *dvtable) {
    if (!dvtable) return;
    for (int i = 0; i < dvtable->dv_num; ++i) {
        free(dvtable->dv[i].dvEntry);
    }
    free(dvtable);
}

//这个函数设置距离矢量表中2个节点之间的链路代价.
//如果这2个节点在表中发现了,并且链路代价也被成功设置了,就返回1,否则返回-1.
int dvtable_setcost(dv_tab *dvtable, int fromNodeID, int toNodeID, unsigned int cost) {
    for (int i = 0; i < dvtable->dv_num; ++i) {
        if (dvtable->dv[i].nodeID == fromNodeID)
            for (int j = 0; j < topology_getNodeNum(); ++j)
                if (dvtable->dv[i].dvEntry[j].nodeID == toNodeID) {
                    dvtable->dv[i].dvEntry[j].cost = cost;
                    return 1;
                }
    }
    return -1;
}

//这个函数返回距离矢量表中2个节点之间的链路代价.
//如果这2个节点在表中发现了,就返回链路代价,否则返回INFINITE_COST.
unsigned int dvtable_getcost(dv_tab *dvtable, int fromNodeID, int toNodeID) {
    if (fromNodeID == toNodeID) return 0;
    for (int i = 0; i < dvtable->dv_num; ++i) {
        if (dvtable->dv[i].nodeID == fromNodeID)
            for (int j = 0; j < topology_getNodeNum(); ++j)
                if (dvtable->dv[i].dvEntry[j].nodeID == toNodeID) {
                    return dvtable->dv[i].dvEntry[j].cost;
                }
    }
    return INFINITE_COST;
}

//这个函数打印距离矢量表的内容.
void dvtable_print(dv_tab *dvtable) {
    if (!dvtable) return;
    printf("[Dv]<dvtable_print> row: %d\n==============================\n", dvtable->dv_num);
    printf("from\t");
    int *nodeArr = topology_getNodeArray();
    for (int i = 0; i < topology_getNodeNum(); ++i) {
        printf("%d\t", nodeArr[i]);
    }
    printf("\n------------------------------\n");\
    for (int i = 0; i < dvtable->dv_num; ++i) {
        printf("%d\t", dvtable->dv[i].nodeID);
        for (int j = 0; j < topology_getNodeNum(); ++j) {
            unsigned int cost = dvtable->dv[i].dvEntry[j].cost;
            cost == INFINITE_COST ? printf("inf\t") : printf("%u\t", cost);
        }
        printf("\n");
    }
    printf("==============================\n");
}
