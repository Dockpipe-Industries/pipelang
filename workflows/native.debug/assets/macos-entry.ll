; Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0

declare i32 @run_checks()

define i32 @main() {
entry:
    %status = call i32 @run_checks()
    ret i32 %status
}
