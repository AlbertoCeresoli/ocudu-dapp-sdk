# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Mini commercial-style Class-A package (canonical multi-vendor template).
#
# Standalone: find_package(OCUDUDapp) — vendor machine after open SDK install.
# In-tree:    parent SDK supplies OCUDU::DAppSDK / ocudu_reference_support /
#             ref_equalizer_cuda_reference_api (no local reference_api.cpp).
#
# Docs: README.md · docs/vendor_toolkit.md · docs/package_config.md

cmake_minimum_required(VERSION 3.18)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(ocudu_dapp_vendor_min VERSION 1 LANGUAGES CXX CUDA)
  # Standalone vendor builds: default Release so OTA Class-A meets deadlines.
  if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release recommended for OTA)" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
    message(STATUS "CMAKE_BUILD_TYPE defaulted to Release (-O3)")
  endif()
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CUDA_STANDARD 17)
  set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_VISIBILITY_PRESET hidden)
  set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES native)
  endif()
  find_package(OCUDUDapp 0.1 CONFIG REQUIRED)
  find_package(CUDAToolkit REQUIRED)
  set(VENDOR_MIN_STANDALONE TRUE)
  set(VENDOR_MIN_DAppSDK OCUDUDapp::DAppSDK)
  set(VENDOR_MIN_SUPPORT OCUDUDapp::reference_support)
  set(VENDOR_MIN_EQ_ALGO OCUDUDapp::ref_equalizer_cuda_algorithm)
  # Standalone: algorithm is open ref_class_a only; wrap via a tiny local API.
  set(VENDOR_MIN_NEED_LOCAL_REF_API TRUE)
else()
  set(VENDOR_MIN_STANDALONE FALSE)
  set(VENDOR_MIN_DAppSDK OCUDU::DAppSDK)
  set(VENDOR_MIN_SUPPORT ocudu_reference_support)
  set(VENDOR_MIN_EQ_ALGO ref_equalizer_cuda_reference_api)
  set(VENDOR_MIN_NEED_LOCAL_REF_API FALSE)
endif()

set(VENDOR_MIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/packages/example_eq_cuda)

if(VENDOR_MIN_NEED_LOCAL_REF_API)
  # When building outside the monorepo, compile a thin host wrapper onto the
  # installed ref_class_a static lib (same symbols open refs use).
  add_library(example_eq_ref_api STATIC ${VENDOR_MIN_SRC}/reference_api.cpp)
  set_target_properties(example_eq_ref_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_link_libraries(example_eq_ref_api PUBLIC ${VENDOR_MIN_EQ_ALGO} ${VENDOR_MIN_DAppSDK})
  set(VENDOR_MIN_EQ_LINK example_eq_ref_api)
else()
  set(VENDOR_MIN_EQ_LINK ${VENDOR_MIN_EQ_ALGO})
endif()

add_library(example_eq_cuda MODULE
  ${VENDOR_MIN_SRC}/module.cpp
  ${VENDOR_MIN_SRC}/module_support.cpp
  ${VENDOR_MIN_SRC}/management.cpp
  ${VENDOR_MIN_SRC}/algorithm.cpp
)
set_target_properties(example_eq_cuda PROPERTIES
  PREFIX ""
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_include_directories(example_eq_cuda PRIVATE ${VENDOR_MIN_SRC})
target_link_libraries(example_eq_cuda PRIVATE
  ${VENDOR_MIN_SUPPORT}
  ${VENDOR_MIN_EQ_LINK}
  ${VENDOR_MIN_DAppSDK}
  CUDA::cudart)

# Manifest next to the MODULE (production pattern).
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/write_package_manifest.cmake")
  set(_manifest_script ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/write_package_manifest.cmake)
elseif(EXISTS "${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake")
  set(_manifest_script ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake)
else()
  set(_manifest_script "")
endif()

if(_manifest_script)
  set(_ex_manifest ${CMAKE_BINARY_DIR}/example_eq_cuda.manifest.json)
  set(_ex_sbom ${CMAKE_BINARY_DIR}/example_eq_cuda.spdx.json)
  add_custom_command(
    OUTPUT ${_ex_manifest} ${_ex_sbom}
    COMMAND ${CMAKE_COMMAND}
      -DOUTPUT=${_ex_manifest}
      -DSBOM=${_ex_sbom}
      -DARTIFACT=$<TARGET_FILE:example_eq_cuda>
      -DPACKAGE_ID=8f0d0001-4f9c-4e31-8c00-00000000e001
      -DGLOBAL_NAME=org.ocudu.example.vendor-min.eq.cuda
      "-DVENDOR=OCUDU example"
      -DVERSION=1
      -DDAPP_CLASS=A
      -DINTERFACE_ID=0x00010004
      -DPACKAGE_PLACEMENT=in_process
      -DINTERFACE_PLACEMENT=in_process
      -DCAPABILITIES=1
      -DBACKEND=cuda
      -DMAX_LAYERS=2
      -P ${_manifest_script}
    DEPENDS example_eq_cuda ${_manifest_script}
    VERBATIM)
  add_custom_target(example_eq_cuda_package ALL DEPENDS ${_ex_manifest} ${_ex_sbom})
endif()

if(BUILD_TESTING AND NOT VENDOR_MIN_STANDALONE)
  if(TARGET module_contract_test)
    add_test(NAME contract.example_eq_cuda
      COMMAND module_contract_test $<TARGET_FILE:example_eq_cuda> 0x00010004 1 1
              org.ocudu.example.vendor-min.eq.cuda "OCUDU example" 1)
  endif()
  if(TARGET certify_class_a_cuda)
    add_test(NAME certify.example_eq_cuda
      COMMAND certify_class_a_cuda $<TARGET_FILE:example_eq_cuda> 0x00010004 --shape-set compact)
    set_tests_properties(certify.example_eq_cuda PROPERTIES LABELS "cuda;class-a;example")
  endif()
endif()
