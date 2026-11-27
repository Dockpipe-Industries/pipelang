# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0

.text
.globl _start
.type _start, %function
_start:
    bl run_checks
    mov x8, #93
    svc #0
.size _start, .-_start
.section .note.GNU-stack,"",%progbits
