//
// Created by ziqi on 2023/4/13.
//

#ifndef LAB04_1_HELPER_H
#define LAB04_1_HELPER_H

#define nstoms(ns) ((double)(ns) / 1000)
#define nstos(ns) ((double)(ns) / 1000000000)
#define stons(s) ((long)(s) * 1000000000)
#define new(struct_t) ((struct_t *) malloc(sizeof(struct_t)))
#define new_n(struct_t, n) ((struct_t *) malloc((n) * sizeof(struct_t)))
#define min(x,y) ((x) > (y) ? (y) : (x))
// limit should be transformed to second first
#define timeout_nano(now, past, limit) (((now) - (past)) >= (limit))

#endif //LAB04_1_HELPER_H
