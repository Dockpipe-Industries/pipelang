// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
// Link and debug-map fixture. Full execution uses the production SDK archive.
.text
.globl _main
_main:
.ifdef X64
    xorl %eax, %eax
.else
    mov w0, #0
.endif
    ret
.globl _pipelang_native_v19_step
_pipelang_native_v19_step:
.ifdef X64
    xorl %eax, %eax
.else
    mov w0, #0
.endif
    ret
.subsections_via_symbols
