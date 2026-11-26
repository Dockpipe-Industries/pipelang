// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0

#if defined(PIPELANG_MODEL_LINKED_RUNTIME) && !PIPELANG_MODEL_LINKED_RUNTIME
#error "task_scope.cpp requires linked runtime mode"
#endif
#ifndef PIPELANG_MODEL_LINKED_RUNTIME
#define PIPELANG_MODEL_LINKED_RUNTIME 1
#endif

#include <pipelang/model/task.hpp>

#define PIPELANG_MODEL_RUNTIME_INLINE
#include <pipelang/model/detail/task_scope_impl.hpp>
#undef PIPELANG_MODEL_RUNTIME_INLINE
