# SPDX-License-Identifier: GPL-3.0-or-later
# MINI (GPL-3.0-or-later, based on CA-72 by Idle Foundry): built inside a Fune checkout (Fune's CMakeLists adds this folder) or
# from the plugins' repository with FUNE_DIR set to a Fune checkout (its sdk/ and src/). Plain C, integers: the same code the FM-1
# image is made of (python3 tools/kpl_build.py plugins/mini/mini.c).
#   build/mini/mini.so        the desktop's plugin
if(NOT FUNE_DIR)
  set(FUNE_DIR "${CMAKE_SOURCE_DIR}")
endif()
set(MINI_OUT "${CMAKE_BINARY_DIR}/mini")

add_library(kreisel_plugin_mini MODULE "${CMAKE_CURRENT_SOURCE_DIR}/mini.c")
target_include_directories(kreisel_plugin_mini PRIVATE "${FUNE_DIR}/sdk" "${FUNE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(kreisel_plugin_mini PROPERTIES PREFIX "" OUTPUT_NAME mini SUFFIX ".so" C_VISIBILITY_PRESET hidden
                      LIBRARY_OUTPUT_DIRECTORY "${MINI_OUT}")

if(TARGET kreisel_plugin_load)
  add_executable(kreisel_mini_test "${CMAKE_CURRENT_SOURCE_DIR}/mini_test.c")
  target_link_libraries(kreisel_mini_test PRIVATE kreisel_plugin_load m)
  target_include_directories(kreisel_mini_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_compile_definitions(kreisel_mini_test PRIVATE KREISEL_MINI="${MINI_OUT}/mini.so")
  add_dependencies(kreisel_mini_test kreisel_plugin_mini)
  add_test(NAME kreisel_mini_test COMMAND kreisel_mini_test)
endif()
if(KREISEL_BUILD_UI AND UNIX AND EXISTS "${FUNE_DIR}/tools/plugin_check/plugin_check.c")   # (Fune's fune_plugin_check, made after)
  add_test(NAME mini_plugin_check COMMAND $<TARGET_FILE:fune_plugin_check> "${MINI_OUT}/mini.so" --quiet)
endif()
