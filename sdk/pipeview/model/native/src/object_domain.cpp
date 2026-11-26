// Copyright PipeLang contributors. SPDX-License-Identifier: Apache-2.0
#if !defined(PIPELANG_MODEL_LINKED_RUNTIME)
#define PIPELANG_MODEL_LINKED_RUNTIME 1
#elif !PIPELANG_MODEL_LINKED_RUNTIME
#error "SDK runtime implementation requires linked runtime mode"
#endif

#include <pipelang/model/object.hpp>

#define PIPELANG_MODEL_RUNTIME_INLINE
#include <pipelang/model/detail/object_domain_impl.hpp>
#undef PIPELANG_MODEL_RUNTIME_INLINE
