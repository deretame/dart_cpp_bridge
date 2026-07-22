#pragma once

// Codegen (libclang) uses clang annotate attributes so markers survive in the AST.
// Unknown [[bridge::*]] is dropped by clang ("unknown attribute ignored") and is
// invisible to clang.cindex — do not rely on it for filtering.
// Normal compile: macros empty (zero warnings / MSVC-friendly).
// Design doc may say BRIDGE_*; this header is the library spelling (DCB_*).
#if defined(DART_CPP_BRIDGE_CODEGEN) || defined(BRIDGE_CODEGEN)
#  define DCB_SYNC __attribute__((annotate("bridge::sync")))
#  define DCB_ASYNC __attribute__((annotate("bridge::async")))
#  define DCB_NORMAL __attribute__((annotate("bridge::normal")))
#  define DCB_EXPORT __attribute__((annotate("bridge::export")))
#else
#  define DCB_SYNC
#  define DCB_ASYNC
#  define DCB_NORMAL
#  define DCB_EXPORT
#endif

#ifndef BRIDGE_SYNC
#  define BRIDGE_SYNC DCB_SYNC
#  define BRIDGE_ASYNC DCB_ASYNC
#  define BRIDGE_NORMAL DCB_NORMAL
#  define BRIDGE_EXPORT DCB_EXPORT
#endif
