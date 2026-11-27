// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#include "NumericRecordAbi.h"

void pipe_check_record_abi(const Point* point, double shift, Point* result, Pair* nested, int64_t* tag)
{
    pipe_adjust_point(point, shift, result);
    pipe_nest_point(point, shift, nested);
    *tag = pipe_point_tag(point);
}
