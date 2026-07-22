#pragma once

// Codegen sees [[bridge::*]]; normal compile expands to empty (no unknown-attribute warnings).
#if defined(DART_CPP_BRIDGE_CODEGEN)
#  define DCB_SYNC [[bridge::sync]]
#  define DCB_ASYNC [[bridge::async]]
#  define DCB_EXPORT [[bridge::export]]
#else
#  define DCB_SYNC
#  define DCB_ASYNC
#  define DCB_EXPORT
#endif
