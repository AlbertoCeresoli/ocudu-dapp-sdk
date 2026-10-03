# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# SRS dataset example. Nothing is built; the tests check the capture, dataset and export
# path against the golden vectors offline and, when the OCUDU loopback agent is available
# (OCUDU_E3AP_TEST_SERVER), a live capture.

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT BUILD_TESTING)
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import asn1tools, h5py, numpy"
  RESULT_VARIABLE _asn1tools_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _asn1tools_rc EQUAL 0)
  if(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES)
    message(FATAL_ERROR "e3_srs_dataset: asn1tools, h5py or numpy not importable — example tests not registered (OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES=ON)")
  endif()
  message(STATUS "e3_srs_dataset: asn1tools, h5py or numpy not importable — example tests not registered")
  return()
endif()

add_test(NAME example.e3_srs_dataset.dataset
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_examples.py -v)
set_tests_properties(example.e3_srs_dataset.dataset PROPERTIES
  LABELS "example;e3;python;offline" TIMEOUT 120
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
if(OCUDU_E3_TEST_ENVIRONMENT)
  set_tests_properties(example.e3_srs_dataset.dataset PROPERTIES ENVIRONMENT "${OCUDU_E3_TEST_ENVIRONMENT}")
endif()
