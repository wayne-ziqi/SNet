
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include "nbrcosttable.h"
#include "../common/constants.h"
#include "../common/helper.h"
#include "../topology/topology.h"

//这个函数动态创建邻居代价表并使用邻居节点ID和直接链路代价初始化该表.
//邻居的节点ID和直接链路代价提取自文件topology.dat. 
nbr_cost_t *nbrcosttable_create(void) {
    nbr_cost_t *nbrCost = new(nbr_cost_t);
    nbrCost->nbr_num = topology_getNbrNum();
    nbrCost->nbrs = new_n(nbr_cost_entry_t, nbrCost->nbr_num);
    // note: the nbrCost table is const-like, should only be used to get cost
    topo_node_t *my_node = get_MyNode();
    int idx = 0;
    iterate_topo_nbr(my_node, nbr) {
        nbrCost->nbrs[idx].nodeID = nbr->neighbor->hostID;
        nbrCost->nbrs[idx].cost = nbr->cost;
        ++idx;
    }
    assert(idx == nbrCost->nbr_num);
    return nbrCost;
}

//这个函数删除邻居代价表.
//它释放所有用于邻居代价表的动态分配内存.
void nbrcosttable_destroy(nbr_cost_t *nct) {
    assert(nct);
    free(nct->nbrs);
    free(nct);
}

//这个函数用于获取邻居的直接链路代价.
//如果邻居节点在表中发现,就返回直接链路代价.否则返回INFINITE_COST.
unsigned int nbrcosttable_getcost(nbr_cost_t *nct, int nodeID) {
    assert(nct);
    for (int i = 0; i < nct->nbr_num; ++i) {
        if (nct->nbrs[i].nodeID == nodeID) return nct->nbrs[i].cost;
    }
    return INFINITE_COST;
}

//这个函数打印邻居代价表的内容.
void nbrcosttable_print(nbr_cost_t *nct) {
    assert(nct);
    printf("[NCT]<nbrcosttable_print> neighbour number: %d\n==============================\n", nct->nbr_num);
    printf("to\t");
    for (int i = 0; i < nct->nbr_num; ++i) {
        printf("%d\t", nct->nbrs[i].nodeID);
    }
    printf("\n------------------------------\ncost\t");
    for (int i = 0; i < nct->nbr_num; ++i) {
        printf("%d\t", nct->nbrs[i].cost);
    }
    printf("\n==============================\n");
}
