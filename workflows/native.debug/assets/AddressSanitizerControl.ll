; Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
target triple = "x86_64-unknown-linux-gnu"

declare ptr @malloc(i64)
declare void @free(ptr)

define i32 @main() sanitize_address {
entry:
  %storage = call ptr @malloc(i64 8)
  %invalid = getelementptr i64, ptr %storage, i64 2
  store volatile i64 42, ptr %invalid, align 8
  call void @free(ptr %storage)
  ret i32 0
}
