# Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
# Linux x86-64 SysV ABI and process-exit fixture for the scalar object.
.text
.globl _start
_start:
    mov $39, %edi
    call Select.Selection.Select
    cmp $42, %rax
    jne failure

    mov $38, %edi
    call Select.Selection.Select
    cmp $7, %rax
    jne failure

    movabs $0x8000000000000000, %rdi
    call Select.Selection.Select
    cmp $7, %rax
    jne failure

    xor %edi, %edi
    jmp exit
failure:
    mov $1, %edi
exit:
    mov $60, %eax
    syscall
.section .note.GNU-stack,"",@progbits
