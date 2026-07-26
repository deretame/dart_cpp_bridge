cmake_minimum_required(VERSION 3.24)
project(hook_demo LANGUAGES C)

add_library(hook_demo SHARED native/demo_add.c)

set_target_properties(hook_demo PROPERTIES
  OUTPUT_NAME hook_demo
  C_STANDARD 11
  C_STANDARD_REQUIRED ON
)
