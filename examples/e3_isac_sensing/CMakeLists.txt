# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# SRS-ISAC over E3AP: a plain process (no dApp LCM) subscribing to the SRS
# channel response. Nothing is built; the test exercises the pipeline on
# synthetic responses and the script's argument parsing.

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT BUILD_TESTING)
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import asn1tools, flatbuffers, numpy"
  RESULT_VARIABLE _deps_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _deps_rc EQUAL 0)
  if(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES)
    message(FATAL_ERROR "e3_isac_sensing: asn1tools/flatbuffers/numpy not importable — example tests not registered (OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES=ON)")
  endif()
  message(STATUS "e3_isac_sensing: asn1tools/flatbuffers/numpy not importable — example tests not registered")
  return()
endif()

add_test(NAME example.e3_isac_sensing.pipeline
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_examples.py -v)
set_tests_properties(example.e3_isac_sensing.pipeline PROPERTIES
  LABELS "example;e3;python;offline" TIMEOUT 120
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
if(OCUDU_E3_TEST_ENVIRONMENT)
  set_tests_properties(example.e3_isac_sensing.pipeline PROPERTIES ENVIRONMENT "${OCUDU_E3_TEST_ENVIRONMENT}")
endif()
