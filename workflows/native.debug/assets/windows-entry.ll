; Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0

declare i32 @run_checks()
declare dllimport void @ExitProcess(i32) noreturn

define void @windows_entry() noreturn {
entry:
    %status = call i32 @run_checks()
    call void @ExitProcess(i32 %status)
    unreachable
}
