# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(NOT DOXYGEN_FOUND)
  message(STATUS "Doxygen not found; SDK documentation target 'doxygen' is disabled")
  return()
endif()

set(DOXYGEN_PROJECT_NAME "OCUDU dApp SDK")
set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
set(DOXYGEN_WARN_LOGFILE "${CMAKE_CURRENT_BINARY_DIR}/doxygen.log")
set(DOXYGEN_GENERATE_TREEVIEW "YES")
set(DOXYGEN_GENERATE_HTML "YES")
set(DOXYGEN_GENERATE_LATEX "NO")
set(DOXYGEN_EXTRACT_ALL "NO")
set(DOXYGEN_QUIET "YES")
set(DOXYGEN_WARN_AS_ERROR "FAIL_ON_WARNINGS")
set(DOXYGEN_WARN_IF_UNDOCUMENTED "NO")
set(DOXYGEN_STRIP_FROM_PATH "${PROJECT_SOURCE_DIR}")
set(DOXYGEN_USE_MDFILE_AS_MAINPAGE "")

if(EXISTS "${CMAKE_SOURCE_DIR}/../ocudu/docs/doxygen/doxygen-awesome.css")
  # Prefer sibling OCUDU tree styling when present; otherwise use defaults.
endif()

set(SDK_DOXYGEN_INPUTS
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/include
)

doxygen_add_docs(doxygen
  ${SDK_DOXYGEN_INPUTS}
  COMMENT "Generating OCUDU dApp SDK documentation"
)
