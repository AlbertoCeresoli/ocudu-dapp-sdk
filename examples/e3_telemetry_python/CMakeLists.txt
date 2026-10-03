# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Python E3AP observer examples. Nothing is built; the tests exercise the
# scripts' argument parsing offline and, when the OCUDU loopback agent is
# available (OCUDU_E3AP_TEST_SERVER), a live setup + subscription round trip.

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT BUILD_TESTING)
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import asn1tools"
  RESULT_VARIABLE _asn1tools_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _asn1tools_rc EQUAL 0)
  if(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES)
    message(FATAL_ERROR "e3_telemetry_python: asn1tools not importable — example tests not registered (OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES=ON)")
  endif()
  message(STATUS "e3_telemetry_python: asn1tools not importable — example tests not registered")
  return()
endif()

add_test(NAME example.e3_telemetry_python.scripts
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_examples.py -v)
set_tests_properties(example.e3_telemetry_python.scripts PROPERTIES
  LABELS "example;e3;python;offline" TIMEOUT 120
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
if(OCUDU_E3_TEST_ENVIRONMENT)
  set_tests_properties(example.e3_telemetry_python.scripts PROPERTIES ENVIRONMENT "${OCUDU_E3_TEST_ENVIRONMENT}")
endif()
