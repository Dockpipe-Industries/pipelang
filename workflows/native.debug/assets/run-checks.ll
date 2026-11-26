; Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0

declare i64 @Select.Selection.Select(i64)

define i32 @run_checks() {
entry:
    %at_threshold = call i64 @Select.Selection.Select(i64 39)
    %threshold_ok = icmp eq i64 %at_threshold, 42
    br i1 %threshold_ok, label %below_threshold, label %failure

below_threshold:
    %below = call i64 @Select.Selection.Select(i64 38)
    %below_ok = icmp eq i64 %below, 7
    br i1 %below_ok, label %minimum, label %failure

minimum:
    %low = call i64 @Select.Selection.Select(i64 -9223372036854775808)
    %low_ok = icmp eq i64 %low, 7
    br i1 %low_ok, label %maximum, label %failure

maximum:
    %high = call i64 @Select.Selection.Select(i64 9223372036854775807)
    %high_ok = icmp eq i64 %high, 42
    br i1 %high_ok, label %success, label %failure

success:
    ret i32 0

failure:
    ret i32 1
}
