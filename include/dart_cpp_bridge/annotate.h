#pragma once

// Codegen sees [[bridge::*]]; normal compile expands to empty (no unknown-attribute warnings).
// Design doc may say BRIDGE_*; this header is the library spelling (DCB_*).
#if defined(DART_CPP_BRIDGE_CODEGEN) || defined(BRIDGE_CODEGEN)
#  define DCB_SYNC [[bridge::sync]]
#  define DCB_ASYNC [[bridge::async]]
#  define DCB_NORMAL [[bridge::normal]]
#  define DCB_EXPORT [[bridge::export]]
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
