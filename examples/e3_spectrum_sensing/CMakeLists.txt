# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Spectrum sensing over E3AP: a plain process (no dApp LCM) subscribing to the
# Class-C spectrum grid. Nothing is built; the test exercises the detector on
# synthetic grids and the script's argument parsing.

find_package(Python3 COMPONENTS Interpreter QUIET)
if(NOT Python3_Interpreter_FOUND OR NOT BUILD_TESTING)
  return()
endif()

execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import asn1tools, flatbuffers, numpy"
  RESULT_VARIABLE _deps_rc OUTPUT_QUIET ERROR_QUIET)
if(NOT _deps_rc EQUAL 0)
  if(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES)
    message(FATAL_ERROR "e3_spectrum_sensing: asn1tools/flatbuffers/numpy not importable — example tests not registered (OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES=ON)")
  endif()
  message(STATUS "e3_spectrum_sensing: asn1tools/flatbuffers/numpy not importable — example tests not registered")
  return()
endif()

add_test(NAME example.e3_spectrum_sensing.detector
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_examples.py -v)
set_tests_properties(example.e3_spectrum_sensing.detector PROPERTIES
  LABELS "example;e3;python;offline" TIMEOUT 120
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
