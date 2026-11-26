// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double wide;
    float narrow;
    uint32_t code;
    int64_t tag;
} Point;

typedef struct {
    Point point;
    float ratio;
} Pair;

void pipe_adjust_point(const Point* point, double shift, Point* output);
void pipe_nest_point(const Point* point, double shift, Pair* output);
int64_t pipe_point_tag(const Point* point);
void pipe_check_record_abi(const Point* point, double shift, Point* result, Pair* nested, int64_t* tag);

#ifdef __cplusplus
}
#endif
