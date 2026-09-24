# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.18)
project(ocudu_dapp_sdk VERSION 0.1.0 LANGUAGES C CXX)

# Default to Release (-O3) so host-path Class-A packages meet PUSCH deadlines.
# Unoptimized builds (empty CMAKE_BUILD_TYPE) were ~10–20× slower and tripped
# the circuit breaker under OTA speed tests for ref_receiver_cpu.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type (Release recommended for OTA)" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
  message(STATUS "CMAKE_BUILD_TYPE defaulted to Release (-O3)")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-Wall>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wextra>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Wpedantic>
    $<$<COMPILE_LANGUAGE:C,CXX>:-Werror>)
endif()

option(OCUDU_DAPP_BUILD_CUDA_EXAMPLES "Build CUDA dApp examples when a CUDA compiler is available" ON)
option(OCUDU_DAPP_BUILD_PYTHON_CLASS_C
  "Build Python Class-C out-of-process sensing example (process + E3 pybind; research only; needs PyTorch for tests)" ON)
option(OCUDU_DAPP_REF_EXTRAS
  "Build process workers, portable E3 consumers, failure-gallery fixtures (OFF = core refs only)" ON)
option(OCUDU_DAPP_REQUIRE_ABI_COMPILER_MATRIX
  "Fail configuration unless GCC and Clang are available for the public ABI release gate" OFF)
option(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES
  "Fail configuration when a Python example suite would silently deregister for a missing dependency" OFF)
option(OCUDU_DAPP_ENABLE_ASAN "Instrument C/C++ references with AddressSanitizer" OFF)
option(OCUDU_DAPP_ENABLE_UBSAN "Instrument C/C++ references with UndefinedBehaviorSanitizer" OFF)
if(OCUDU_DAPP_ENABLE_ASAN OR OCUDU_DAPP_ENABLE_UBSAN)
  if(NOT CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "SDK sanitizer gates require GCC or Clang")
  endif()
  set(OCUDU_DAPP_SANITIZERS "")
  if(OCUDU_DAPP_ENABLE_ASAN)
    list(APPEND OCUDU_DAPP_SANITIZERS address)
  endif()
  if(OCUDU_DAPP_ENABLE_UBSAN)
    list(APPEND OCUDU_DAPP_SANITIZERS undefined)
  endif()
  list(JOIN OCUDU_DAPP_SANITIZERS "," OCUDU_DAPP_SANITIZER_LIST)
  add_compile_options(
    $<$<COMPILE_LANGUAGE:C,CXX>:-fsanitize=${OCUDU_DAPP_SANITIZER_LIST}>
    $<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>)
  add_link_options(-fsanitize=${OCUDU_DAPP_SANITIZER_LIST})
  message(STATUS "SDK sanitizers enabled: ${OCUDU_DAPP_SANITIZER_LIST}")
endif()
if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
  include(CheckLanguage)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    find_package(CUDAToolkit REQUIRED)
    set(CMAKE_CUDA_STANDARD 17)
    set(CMAKE_CUDA_STANDARD_REQUIRED ON)
  else()
    message(STATUS "CUDA compiler not found; CUDA examples are disabled")
    set(OCUDU_DAPP_BUILD_CUDA_EXAMPLES OFF)
  endif()
endif()

find_package(OCUDUDAppSDK 1 CONFIG REQUIRED)
include(GNUInstallDirs)
include(CTest)

find_package(FlatBuffers CONFIG QUIET)
find_program(FLATC_EXECUTABLE flatc)
if(FLATC_EXECUTABLE AND TARGET flatbuffers::flatbuffers)
  set(CLASS_C_GENERATED_DIR ${PROJECT_BINARY_DIR}/generated)
  set(CLASS_C_GENERATED_HEADER ${CLASS_C_GENERATED_DIR}/class-c-publication-v1_generated.h)
  add_custom_command(
    OUTPUT ${CLASS_C_GENERATED_HEADER}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CLASS_C_GENERATED_DIR}
    COMMAND ${FLATC_EXECUTABLE} --cpp --scoped-enums -o ${CLASS_C_GENERATED_DIR}
      ${OCUDUDAppSDK_CLASS_C_SCHEMA}
    DEPENDS ${OCUDUDAppSDK_CLASS_C_SCHEMA}
    COMMENT "Generating private Class-C metadata bindings")
  add_custom_target(ocudu_reference_class_c_schema DEPENDS ${CLASS_C_GENERATED_HEADER})
  add_library(ocudu_reference_class_c_types INTERFACE)
  set_target_properties(ocudu_reference_class_c_types PROPERTIES EXPORT_NAME reference_class_c_types)
  add_library(OCUDUDapp::reference_class_c_types ALIAS ocudu_reference_class_c_types)
  add_dependencies(ocudu_reference_class_c_types ocudu_reference_class_c_schema)
  target_include_directories(ocudu_reference_class_c_types INTERFACE
    $<BUILD_INTERFACE:${CLASS_C_GENERATED_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/ocudu_dapp_sdk/generated>)
  target_link_libraries(ocudu_reference_class_c_types INTERFACE flatbuffers::flatbuffers)
else()
  message(FATAL_ERROR "Class-C process examples require flatc and the FlatBuffers CMake package")
endif()

# Shared include dirs for installable toolkit targets (build tree + install tree).
set(OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD ${PROJECT_SOURCE_DIR}/include)
set(OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL ${CMAKE_INSTALL_INCLUDEDIR})

# module_support + Class-C config skeleton (single multi-vendor management basis).
add_library(ocudu_reference_support STATIC
  reference/common/module_support.cpp
  reference/common/class_c_config.cpp)
set_target_properties(ocudu_reference_support PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  EXPORT_NAME reference_support)
add_library(OCUDUDapp::reference_support ALIAS ocudu_reference_support)
target_include_directories(ocudu_reference_support PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_reference_support PUBLIC OCUDU::DAppSDK)

add_library(ocudu_reference_class_c_algorithms STATIC reference/common/class_c_algorithms.cpp)
set_target_properties(ocudu_reference_class_c_algorithms PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ocudu_reference_class_c_algorithms PROPERTIES EXPORT_NAME reference_class_c_algorithms)
add_library(OCUDUDapp::reference_class_c_algorithms ALIAS ocudu_reference_class_c_algorithms)
target_include_directories(ocudu_reference_class_c_algorithms PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_reference_class_c_algorithms PUBLIC OCUDU::DAppSDK)

add_library(ocudu_reference_phy_algorithms STATIC reference/common/reference_phy_algorithms.cpp)
set_target_properties(ocudu_reference_phy_algorithms PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ocudu_reference_phy_algorithms PROPERTIES EXPORT_NAME reference_phy_algorithms)
add_library(OCUDUDapp::reference_phy_algorithms ALIAS ocudu_reference_phy_algorithms)
target_include_directories(ocudu_reference_phy_algorithms PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_reference_phy_algorithms PUBLIC OCUDU::DAppSDK)

add_library(ocudu_reference_scheduler_algorithm STATIC
  reference/common/reference_scheduler_pipeline.cpp
  reference/common/reference_scheduler_tables.cpp
  reference/common/reference_scheduler_olla.cpp)
set_target_properties(ocudu_reference_scheduler_algorithm PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ocudu_reference_scheduler_algorithm PROPERTIES EXPORT_NAME reference_scheduler_algorithm)
add_library(OCUDUDapp::reference_scheduler_algorithm ALIAS ocudu_reference_scheduler_algorithm)
target_include_directories(ocudu_reference_scheduler_algorithm PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_reference_scheduler_algorithm PUBLIC OCUDU::DAppSDK)

add_library(ocudu_process_client STATIC reference/process/process_client.cpp)
set_target_properties(ocudu_process_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ocudu_process_client PROPERTIES EXPORT_NAME process_client)
add_library(OCUDUDapp::process_client ALIAS ocudu_process_client)
target_include_directories(ocudu_process_client PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_process_client PUBLIC OCUDU::DAppSDK ocudu_reference_class_c_types)

add_library(ocudu_reference_class_c_input STATIC reference/process/class_c_input.cpp)
set_target_properties(ocudu_reference_class_c_input PROPERTIES POSITION_INDEPENDENT_CODE ON)
set_target_properties(ocudu_reference_class_c_input PROPERTIES EXPORT_NAME reference_class_c_input)
add_library(OCUDUDapp::reference_class_c_input ALIAS ocudu_reference_class_c_input)
target_include_directories(ocudu_reference_class_c_input PUBLIC
  $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
  $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
target_link_libraries(ocudu_reference_class_c_input PUBLIC OCUDU::DAppSDK ocudu_reference_class_c_types)

if(OCUDU_DAPP_REF_EXTRAS AND OCUDUDAppSDK_E3_MANAGEMENT_AVAILABLE)
  # PIC: must be linkable into shared modules (Python pybind extension).
  add_library(ocudu_portable_e3_consumer STATIC reference/portable/portable_e3_consumer.cpp)
  set_target_properties(ocudu_portable_e3_consumer PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME portable_e3_consumer)
  add_library(OCUDUDapp::portable_e3_consumer ALIAS ocudu_portable_e3_consumer)
  target_include_directories(ocudu_portable_e3_consumer PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(ocudu_portable_e3_consumer PUBLIC OCUDU::DAppManagementClient)

  foreach(interface IN ITEMS spectrum srs_isac)
    if(interface STREQUAL "spectrum")
      set(interface_id 0x00010001)
    else()
      set(interface_id 0x00010002)
    endif()
    add_executable(ref_${interface}_e3_cpu reference/portable/consumer_main.cpp)
    target_compile_definitions(ref_${interface}_e3_cpu PRIVATE OCUDU_REFERENCE_PORTABLE_INTERFACE=${interface_id})
    target_link_libraries(ref_${interface}_e3_cpu PRIVATE ocudu_portable_e3_consumer ocudu_reference_class_c_input
      ocudu_reference_class_c_algorithms)
  endforeach()
elseif(NOT OCUDU_DAPP_REF_EXTRAS)
  message(STATUS "OCUDU_DAPP_REF_EXTRAS=OFF: skipping portable E3 consumers, process workers, failure gallery")
endif()

function(add_reference_dapp target source interface_id dapp_class placement_mask)
  add_library(${target} MODULE ${source})
  set_target_properties(${target} PROPERTIES PREFIX "")
  target_link_libraries(${target} PRIVATE ocudu_reference_support)
  if(BUILD_TESTING)
    add_test(NAME contract.${target}
      COMMAND module_contract_test $<TARGET_FILE:${target}> ${interface_id} ${dapp_class} ${placement_mask})
  endif()
endfunction()

add_reference_dapp(ref_spectrum_cpu reference/spectrum/module.cpp 0x00010001 3 3)
add_reference_dapp(ref_srs_isac_cpu reference/srs_isac/module.cpp 0x00010002 3 3)
target_link_libraries(ref_spectrum_cpu PRIVATE ocudu_reference_class_c_algorithms)
target_link_libraries(ref_srs_isac_cpu PRIVATE ocudu_reference_class_c_algorithms)
add_reference_dapp(ref_channel_estimator_cpu reference/channel_estimator/module.cpp 0x00010003 1 1)
add_reference_dapp(ref_equalizer_cpu reference/equalizer/module.cpp 0x00010004 1 1)
add_reference_dapp(ref_receiver_cpu reference/receiver/module.cpp 0x00010005 1 1)
target_link_libraries(ref_channel_estimator_cpu PRIVATE ocudu_reference_phy_algorithms)
target_link_libraries(ref_equalizer_cpu PRIVATE ocudu_reference_phy_algorithms)
target_link_libraries(ref_receiver_cpu PRIVATE ocudu_reference_phy_algorithms)
add_reference_dapp(ref_scheduler_cpu reference/scheduler/module.cpp 0x00010006 2 1)
target_link_libraries(ref_scheduler_cpu PRIVATE ocudu_reference_scheduler_algorithm)

if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
  function(add_reference_cuda_algorithm target source)
    add_library(${target} STATIC ${source})
    set_target_properties(${target} PROPERTIES
      POSITION_INDEPENDENT_CODE ON
      CUDA_SEPARABLE_COMPILATION ON)
    target_include_directories(${target} PUBLIC
      $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
      $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
    target_link_libraries(${target} PUBLIC OCUDU::DAppSDK CUDA::cudart)
  endfunction()

  add_reference_cuda_algorithm(ref_channel_estimator_cuda_algorithm reference/channel_estimator_cuda/algorithm.cu)
  # EQ/RX cores export only ocudu::dapp::ref_class_a::* (vendor-safe; no equalize_cuda ODR clash).
  add_reference_cuda_algorithm(ref_equalizer_cuda_algorithm reference/equalizer_cuda/algorithm.cu)
  add_reference_cuda_algorithm(ref_receiver_cuda_algorithm reference/receiver_cuda/algorithm.cu)
  set_target_properties(ref_equalizer_cuda_algorithm PROPERTIES EXPORT_NAME ref_equalizer_cuda_algorithm)
  set_target_properties(ref_receiver_cuda_algorithm PROPERTIES EXPORT_NAME ref_receiver_cuda_algorithm)
  set_target_properties(ref_channel_estimator_cuda_algorithm PROPERTIES EXPORT_NAME ref_channel_estimator_cuda_algorithm)
  add_library(OCUDUDapp::ref_equalizer_cuda_algorithm ALIAS ref_equalizer_cuda_algorithm)
  add_library(OCUDUDapp::ref_receiver_cuda_algorithm ALIAS ref_receiver_cuda_algorithm)
  add_library(OCUDUDapp::ref_channel_estimator_cuda_algorithm ALIAS ref_channel_estimator_cuda_algorithm)

  # Open-reference ABI wrappers (reference::equalize_cuda / receive_cuda) for ref packages only.
  # OBJECT libs so CUDA device link still sees ref_*_cuda_algorithm objects through consumers.
  add_library(ref_equalizer_cuda_reference_api OBJECT reference/equalizer_cuda/reference_api.cpp)
  set_target_properties(ref_equalizer_cuda_reference_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(ref_equalizer_cuda_reference_api PUBLIC
    $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
    $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
  target_link_libraries(ref_equalizer_cuda_reference_api PUBLIC ref_equalizer_cuda_algorithm)

  add_library(ref_receiver_cuda_reference_api OBJECT reference/receiver_cuda/reference_api.cpp)
  set_target_properties(ref_receiver_cuda_reference_api PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(ref_receiver_cuda_reference_api PUBLIC
    $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
    $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
  target_link_libraries(ref_receiver_cuda_reference_api PUBLIC ref_receiver_cuda_algorithm)

  add_reference_cuda_algorithm(ref_spectrum_cuda_algorithm reference/spectrum_cuda/algorithm.cu)
  add_reference_cuda_algorithm(ref_srs_isac_cuda_algorithm reference/srs_isac_cuda/algorithm.cu)
  set_target_properties(ref_spectrum_cuda_algorithm PROPERTIES EXPORT_NAME ref_spectrum_cuda_algorithm)
  set_target_properties(ref_srs_isac_cuda_algorithm PROPERTIES EXPORT_NAME ref_srs_isac_cuda_algorithm)
  add_library(OCUDUDapp::ref_spectrum_cuda_algorithm ALIAS ref_spectrum_cuda_algorithm)
  add_library(OCUDUDapp::ref_srs_isac_cuda_algorithm ALIAS ref_srs_isac_cuda_algorithm)

  add_library(ocudu_process_cuda_client STATIC reference/process/process_cuda_client.cpp)
  set_target_properties(ocudu_process_cuda_client PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    EXPORT_NAME process_cuda_client)
  add_library(OCUDUDapp::process_cuda_client ALIAS ocudu_process_cuda_client)
  target_include_directories(ocudu_process_cuda_client PUBLIC
    $<BUILD_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_BUILD}>
    $<INSTALL_INTERFACE:${OCUDU_DAPP_TOOLKIT_INCLUDE_INSTALL}>)
  target_link_libraries(ocudu_process_cuda_client PUBLIC ocudu_process_client CUDA::cudart)

  add_library(ref_spectrum_cuda MODULE reference/spectrum_cuda/module.cpp)
  set_target_properties(ref_spectrum_cuda PROPERTIES PREFIX "")
  target_link_libraries(ref_spectrum_cuda PRIVATE ocudu_reference_support ref_spectrum_cuda_algorithm)

  add_library(ref_srs_isac_cuda MODULE reference/srs_isac_cuda/module.cpp)
  set_target_properties(ref_srs_isac_cuda PROPERTIES PREFIX "")
  target_link_libraries(ref_srs_isac_cuda PRIVATE
    ocudu_reference_support ocudu_reference_class_c_algorithms ref_srs_isac_cuda_algorithm)

  add_reference_dapp(ref_channel_estimator_cuda
    "reference/channel_estimator_cuda/module.cpp;reference/channel_estimator_cuda/module_support.cpp"
    0x00010003 1 1)
  add_reference_dapp(ref_equalizer_cuda
    "reference/equalizer_cuda/module.cpp;reference/equalizer_cuda/module_support.cpp" 0x00010004 1 1)
  add_reference_dapp(ref_receiver_cuda
    "reference/receiver_cuda/module.cpp;reference/receiver_cuda/module_support.cpp" 0x00010005 1 1)
  target_link_libraries(ref_channel_estimator_cuda PRIVATE ref_channel_estimator_cuda_algorithm)
  target_link_libraries(ref_equalizer_cuda PRIVATE ref_equalizer_cuda_reference_api)
  target_link_libraries(ref_receiver_cuda PRIVATE ref_receiver_cuda_reference_api)

  if(BUILD_TESTING)
    add_reference_cuda_algorithm(certifier_bad_bounds_cuda_algorithm
      tests/bad_channel_estimator_cuda_algorithm.cu)
    add_reference_cuda_algorithm(certifier_bad_storage_cuda_algorithm
      tests/bad_channel_estimator_cuda_algorithm.cu)
    target_compile_definitions(certifier_bad_storage_cuda_algorithm PRIVATE
      OCUDU_CERTIFIER_BAD_STORAGE_SUBSTITUTION=1)
    add_library(certifier_bad_bounds_cuda MODULE
      reference/channel_estimator_cuda/module.cpp reference/channel_estimator_cuda/module_support.cpp)
    add_library(certifier_bad_storage_cuda MODULE
      reference/channel_estimator_cuda/module.cpp reference/channel_estimator_cuda/module_support.cpp)
    add_library(certifier_bad_layers_cuda MODULE
      reference/channel_estimator_cuda/module.cpp reference/channel_estimator_cuda/module_support.cpp)
    add_library(certifier_bad_transform_cuda MODULE
      reference/channel_estimator_cuda/module.cpp reference/channel_estimator_cuda/module_support.cpp)
    set_target_properties(certifier_bad_bounds_cuda certifier_bad_storage_cuda certifier_bad_layers_cuda
      certifier_bad_transform_cuda PROPERTIES PREFIX "")
    target_compile_definitions(certifier_bad_layers_cuda PRIVATE OCUDU_REFERENCE_CLASS_A_MAX_LAYERS=3)
    target_compile_definitions(certifier_bad_transform_cuda PRIVATE
      OCUDU_REFERENCE_CLASS_A_PROFILE_FLAGS=3)
    target_link_libraries(certifier_bad_bounds_cuda PRIVATE
      ocudu_reference_support certifier_bad_bounds_cuda_algorithm)
    target_link_libraries(certifier_bad_storage_cuda PRIVATE
      ocudu_reference_support certifier_bad_storage_cuda_algorithm)
    target_link_libraries(certifier_bad_layers_cuda PRIVATE
      ocudu_reference_support ref_channel_estimator_cuda_algorithm)
    target_link_libraries(certifier_bad_transform_cuda PRIVATE
      ocudu_reference_support ref_channel_estimator_cuda_algorithm)
  endif()

  add_executable(make_receiver_cuda_model tools/make_receiver_cuda_model.cpp)
  target_include_directories(make_receiver_cuda_model PRIVATE ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(make_receiver_cuda_model PRIVATE OCUDU::DAppSDK)

  add_executable(certify_class_a_cuda tools/certify_class_a_cuda.cu)
  target_link_libraries(certify_class_a_cuda PRIVATE OCUDU::DAppSDK CUDA::cudart ${CMAKE_DL_LIBS})

  # Central L1 validation suite (dlopen any Class-A EQ/RX package — open or vendor).
  add_executable(l1_validation_suite tools/l1_validation/l1_suite.cu)
  target_link_libraries(l1_validation_suite PRIVATE OCUDU::DAppSDK CUDA::cudart ${CMAKE_DL_LIBS})
  set_target_properties(l1_validation_suite PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${PROJECT_BINARY_DIR}
    CUDA_SEPARABLE_COMPILATION OFF)
endif()

function(add_reference_process target interface_id)
  add_executable(${target} reference/process/worker_main.cpp)
  target_compile_definitions(${target} PRIVATE OCUDU_REFERENCE_INTERFACE_ID=${interface_id})
  target_link_libraries(${target} PRIVATE ocudu_process_client ocudu_reference_class_c_input ocudu_reference_support
    ocudu_reference_class_c_algorithms)
endfunction()

# Process workers / failure gallery / multi-interface fixtures (optional extras).
if(OCUDU_DAPP_REF_EXTRAS)
  add_reference_process(ref_spectrumd_cpu 0x00010001)
  add_reference_process(ref_spectrum_observerd_cpu 0x00010001)
  target_compile_definitions(ref_spectrum_observerd_cpu PRIVATE OCUDU_REFERENCE_OBSERVER_ONLY=1)
  add_reference_process(ref_srs_isacd_cpu 0x00010002)

  if(BUILD_TESTING)
    add_reference_process(test_srs_dmrs_fusiond_cpu 0x00010002)
    target_compile_definitions(test_srs_dmrs_fusiond_cpu PRIVATE OCUDU_REFERENCE_OPTIONAL_DMRS_INTERFACE_ID=0x7f010002)

    add_library(test_srs_dmrs_fusion_cpu MODULE tests/srs_dmrs_fusion_module.cpp)
    set_target_properties(test_srs_dmrs_fusion_cpu PROPERTIES PREFIX "")
    target_link_libraries(test_srs_dmrs_fusion_cpu PRIVATE ocudu_reference_support)
    add_test(NAME contract.test_srs_dmrs_fusion_cpu
      COMMAND module_contract_test $<TARGET_FILE:test_srs_dmrs_fusion_cpu> fusion)

    add_library(test_joint_spectrum_scheduler_cpu MODULE tests/joint_spectrum_scheduler_module.cpp)
    set_target_properties(test_joint_spectrum_scheduler_cpu PROPERTIES PREFIX "")
    target_link_libraries(test_joint_spectrum_scheduler_cpu PRIVATE
      ocudu_reference_support
      ocudu_reference_class_c_algorithms
      ocudu_reference_scheduler_algorithm)
    add_test(NAME contract.test_joint_spectrum_scheduler_cpu
      COMMAND module_contract_test $<TARGET_FILE:test_joint_spectrum_scheduler_cpu> joint)

    foreach(mode IN ITEMS exception invalid deadline)
      if(mode STREQUAL "exception")
        set(mode_id 1)
      elseif(mode STREQUAL "invalid")
        set(mode_id 2)
      else()
        set(mode_id 3)
      endif()
      set(target failure_channel_estimator_${mode}_cpu)
      add_library(${target} MODULE tests/failure_channel_estimator.cpp)
      set_target_properties(${target} PROPERTIES PREFIX "")
      target_compile_definitions(${target} PRIVATE
        OCUDU_FAILURE_MODE=${mode_id}
        OCUDU_FAILURE_GLOBAL_NAME="org.ocudu.test.failure-gallery.channel-estimator.${mode}")
      target_link_libraries(${target} PRIVATE ocudu_reference_support)
    endforeach()

    add_executable(failure_spectrumd_invalid_cpu reference/process/worker_main.cpp)
    target_compile_definitions(failure_spectrumd_invalid_cpu PRIVATE
      OCUDU_REFERENCE_INTERFACE_ID=0x00010001 OCUDU_REFERENCE_INVALID_PROCESS_RESULT=1)
    target_link_libraries(failure_spectrumd_invalid_cpu PRIVATE
      ocudu_process_client ocudu_reference_class_c_input ocudu_reference_support ocudu_reference_class_c_algorithms)
  endif()

  if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
    add_executable(ref_spectrumd_cuda reference/process/worker_main.cpp)
    target_compile_definitions(ref_spectrumd_cuda PRIVATE
      OCUDU_REFERENCE_INTERFACE_ID=0x00010001 OCUDU_REFERENCE_CUDA_PROCESS=1)
    target_link_libraries(ref_spectrumd_cuda PRIVATE
      ocudu_process_client ocudu_process_cuda_client ocudu_reference_class_c_input ocudu_reference_support
      ref_spectrum_cuda_algorithm)
    set_target_properties(ref_spectrumd_cuda PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)

    add_executable(ref_srs_isacd_cuda reference/process/worker_main.cpp)
    target_compile_definitions(ref_srs_isacd_cuda PRIVATE
      OCUDU_REFERENCE_INTERFACE_ID=0x00010002 OCUDU_REFERENCE_CUDA_PROCESS=1)
    target_link_libraries(ref_srs_isacd_cuda PRIVATE
      ocudu_process_client ocudu_process_cuda_client ocudu_reference_class_c_input ocudu_reference_support
      ocudu_reference_class_c_algorithms ref_srs_isac_cuda_algorithm)
    set_target_properties(ref_srs_isacd_cuda PROPERTIES CUDA_RESOLVE_DEVICE_SYMBOLS ON)
  endif()
endif() # OCUDU_DAPP_REF_EXTRAS

set(REFERENCE_MANIFEST_WRITER ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake)
set(REFERENCE_MANIFEST_VERIFIER ${PROJECT_SOURCE_DIR}/cmake/verify_package_manifest.cmake)

function(add_reference_manifest target package_id global_name dapp_class interface_id package_placement
    interface_placement capabilities backend)
  cmake_parse_arguments(ARG "" "INTERFACE_COUNT" "ADDITIONAL_ARGUMENTS;EXPECTED_INTERFACE_IDS;EXPECTED_BINDINGS" ${ARGN})
  set(writer_arguments)
  set(verifier_arguments)
  if(ARG_INTERFACE_COUNT)
    list(APPEND writer_arguments -DINTERFACE_COUNT=${ARG_INTERFACE_COUNT})
    list(APPEND verifier_arguments -DEXPECTED_INTERFACE_COUNT=${ARG_INTERFACE_COUNT})
  endif()
  if(ARG_ADDITIONAL_ARGUMENTS)
    list(APPEND writer_arguments ${ARG_ADDITIONAL_ARGUMENTS})
  endif()
  if(ARG_EXPECTED_INTERFACE_IDS)
    list(APPEND verifier_arguments "-DEXPECTED_INTERFACE_IDS=${ARG_EXPECTED_INTERFACE_IDS}")
  endif()
  if(ARG_EXPECTED_BINDINGS)
    list(APPEND verifier_arguments "-DEXPECTED_BINDINGS=${ARG_EXPECTED_BINDINGS}")
  endif()
  set(manifest ${PROJECT_BINARY_DIR}/${target}.manifest.json)
  set(sbom ${PROJECT_BINARY_DIR}/${target}.spdx.json)
  add_custom_command(
    OUTPUT ${manifest} ${sbom}
    COMMAND ${CMAKE_COMMAND}
      -DOUTPUT=${manifest}
      -DSBOM=${sbom}
      -DARTIFACT=$<TARGET_FILE:${target}>
      -DPACKAGE_ID=${package_id}
      -DGLOBAL_NAME=${global_name}
      "-DVENDOR=OCUDU reference"
      -DVERSION=v1
      -DDAPP_CLASS=${dapp_class}
      -DINTERFACE_ID=${interface_id}
      -DPACKAGE_PLACEMENT=${package_placement}
      -DINTERFACE_PLACEMENT=${interface_placement}
      -DCAPABILITIES=${capabilities}
      -DBACKEND=${backend}
      ${writer_arguments}
      -P ${REFERENCE_MANIFEST_WRITER}
    DEPENDS ${target} ${REFERENCE_MANIFEST_WRITER}
    VERBATIM
    COMMENT "Hashing and packaging ${target}")
  add_custom_target(${target}_manifest ALL DEPENDS ${manifest} ${sbom})
  if(BUILD_TESTING)
    add_test(NAME manifest.${target}
      COMMAND ${CMAKE_COMMAND} -DMANIFEST=${manifest} -DSBOM=${sbom} -DARTIFACT=$<TARGET_FILE:${target}>
        -DEXPECTED_CAPABILITIES=${capabilities}
        ${verifier_arguments}
        -P ${REFERENCE_MANIFEST_VERIFIER})
  endif()
endfunction()

add_reference_manifest(ref_spectrum_cpu 8f0d0001-4f9c-4e31-8c00-000000000001
  org.ocudu.reference.spectrum.cpu C 0x00010001 in_process both 7 cpu)
add_reference_manifest(ref_srs_isac_cpu 8f0d0001-4f9c-4e31-8c00-000000000002
  org.ocudu.reference.srs-isac.cpu C 0x00010002 in_process both 0 cpu)
add_reference_manifest(ref_channel_estimator_cpu 8f0d0001-4f9c-4e31-8c00-000000000003
  org.ocudu.reference.channel-estimator.cpu A 0x00010003 in_process in_process 0 cpu)
add_reference_manifest(ref_equalizer_cpu 8f0d0001-4f9c-4e31-8c00-000000000004
  org.ocudu.reference.equalizer.cpu A 0x00010004 in_process in_process 0 cpu)
add_reference_manifest(ref_receiver_cpu 8f0d0001-4f9c-4e31-8c00-000000000005
  org.ocudu.reference.receiver.cpu A 0x00010005 in_process in_process 0 cpu)
add_reference_manifest(ref_scheduler_cpu 8f0d0001-4f9c-4e31-8c00-000000000006
  org.ocudu.reference.scheduler.cpu B 0x00010006 in_process in_process 0 cpu)
if(OCUDU_DAPP_REF_EXTRAS AND BUILD_TESTING)
  add_reference_manifest(test_srs_dmrs_fusiond_cpu 8f0d0001-4f9c-4e31-8c00-000000000407
    org.ocudu.test.srs-dmrs-fusion.process.cpu C 0x00010002 out_of_process out_of_process 0 cpu
    INTERFACE_COUNT 2
    ADDITIONAL_ARGUMENTS
      -DINTERFACE_ID_2=0x7f010002
      -DDAPP_CLASS_2=C
      -DINTERFACE_PLACEMENT_2=out_of_process
      -DCAPABILITIES_2=0
      -DBINDING_2=optional
    EXPECTED_INTERFACE_IDS 0x00010002 0x7f010002
    EXPECTED_BINDINGS required optional)
  add_reference_manifest(test_srs_dmrs_fusion_cpu 8f0d0001-4f9c-4e31-8c00-000000000406
    org.ocudu.test.srs-dmrs-fusion.cpu C 0x00010002 in_process in_process 0 cpu
    INTERFACE_COUNT 2
    ADDITIONAL_ARGUMENTS
      -DINTERFACE_ID_2=0x7f010002
      -DDAPP_CLASS_2=C
      -DINTERFACE_PLACEMENT_2=in_process
      -DCAPABILITIES_2=0
      -DBINDING_2=optional
    EXPECTED_INTERFACE_IDS 0x00010002 0x7f010002
    EXPECTED_BINDINGS required optional)
  add_reference_manifest(test_joint_spectrum_scheduler_cpu 8f0d0001-4f9c-4e31-8c00-000000000405
    org.ocudu.test.joint-spectrum-scheduler.cpu C 0x00010001 in_process in_process 1 cpu
    INTERFACE_COUNT 2
    ADDITIONAL_ARGUMENTS
      -DINTERFACE_ID_2=0x00010006
      -DDAPP_CLASS_2=B
      -DINTERFACE_PLACEMENT_2=in_process
      -DCAPABILITIES_2=0
    EXPECTED_INTERFACE_IDS 0x00010001 0x00010006)
  add_reference_manifest(failure_channel_estimator_exception_cpu 8f0d0001-4f9c-4e31-8c00-000000000401
    org.ocudu.test.failure-gallery.channel-estimator.exception A 0x00010003 in_process in_process 0 cpu)
  add_reference_manifest(failure_channel_estimator_invalid_cpu 8f0d0001-4f9c-4e31-8c00-000000000402
    org.ocudu.test.failure-gallery.channel-estimator.invalid A 0x00010003 in_process in_process 0 cpu)
  add_reference_manifest(failure_channel_estimator_deadline_cpu 8f0d0001-4f9c-4e31-8c00-000000000403
    org.ocudu.test.failure-gallery.channel-estimator.deadline A 0x00010003 in_process in_process 0 cpu)
  add_reference_manifest(failure_spectrumd_invalid_cpu 8f0d0001-4f9c-4e31-8c00-000000000404
    org.ocudu.test.failure-gallery.spectrum.invalid-result C 0x00010001 out_of_process out_of_process 7 cpu)
endif()

if(OCUDU_DAPP_REF_EXTRAS)
  add_reference_manifest(ref_spectrumd_cpu 8f0d0001-4f9c-4e31-8c00-000000000101
    org.ocudu.reference.spectrum.process.cpu C 0x00010001 out_of_process out_of_process 7 cpu)
  add_reference_manifest(ref_srs_isacd_cpu 8f0d0001-4f9c-4e31-8c00-000000000102
    org.ocudu.reference.srs-isac.process.cpu C 0x00010002 out_of_process out_of_process 0 cpu)
  add_reference_manifest(ref_spectrum_observerd_cpu 8f0d0001-4f9c-4e31-8c00-000000000103
    org.ocudu.reference.spectrum-observer.process.cpu C 0x00010001 out_of_process out_of_process 0 cpu)
endif()

if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
  add_reference_manifest(ref_spectrum_cuda 8f0d0001-4f9c-4e31-8c00-000000000201
    org.ocudu.reference.spectrum.cuda C 0x00010001 in_process in_process 1 cuda)
  add_reference_manifest(ref_srs_isac_cuda 8f0d0001-4f9c-4e31-8c00-000000000202
    org.ocudu.reference.srs-isac.cuda C 0x00010002 in_process in_process 0 cuda)
  add_reference_manifest(ref_channel_estimator_cuda 8f0d0001-4f9c-4e31-8c00-000000000203
    org.ocudu.reference.channel-estimator.cuda A 0x00010003 in_process in_process 1 cuda)
  add_reference_manifest(ref_equalizer_cuda 8f0d0001-4f9c-4e31-8c00-000000000204
    org.ocudu.reference.equalizer.cuda A 0x00010004 in_process in_process 1 cuda)
  add_reference_manifest(ref_receiver_cuda 8f0d0001-4f9c-4e31-8c00-000000000205
    org.ocudu.reference.receiver.cuda A 0x00010005 in_process in_process 1 cuda)
  if(OCUDU_DAPP_REF_EXTRAS)
    add_reference_manifest(ref_spectrumd_cuda 8f0d0001-4f9c-4e31-8c00-000000000301
      org.ocudu.reference.spectrum.process.cuda C 0x00010001 out_of_process out_of_process 7 cuda)
    add_reference_manifest(ref_srs_isacd_cuda 8f0d0001-4f9c-4e31-8c00-000000000302
      org.ocudu.reference.srs-isac.process.cuda C 0x00010002 out_of_process out_of_process 0 cuda)
  endif()
endif()

if(BUILD_TESTING)
  find_package(GTest REQUIRED)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  find_program(OCUDU_DAPP_GCC_EXECUTABLE gcc)
  find_program(OCUDU_DAPP_GXX_EXECUTABLE g++)
  find_program(OCUDU_DAPP_CLANG_EXECUTABLE clang)
  find_program(OCUDU_DAPP_CLANGXX_EXECUTABLE clang++)
  if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND
     OCUDU_DAPP_GCC_EXECUTABLE AND OCUDU_DAPP_GXX_EXECUTABLE AND
     OCUDU_DAPP_CLANG_EXECUTABLE AND OCUDU_DAPP_CLANGXX_EXECUTABLE)
    get_target_property(OCUDU_DAPP_PUBLIC_INCLUDE_DIRS OCUDU::DAppSDK INTERFACE_INCLUDE_DIRECTORIES)
    list(GET OCUDU_DAPP_PUBLIC_INCLUDE_DIRS 0 OCUDU_DAPP_PUBLIC_INCLUDE_DIR)
    add_test(NAME abi.public_v1_compiler_matrix
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/abi_compiler_matrix_test.py
        --source ${PROJECT_SOURCE_DIR}/tests/abi_layout_dump.cpp
        --expected ${PROJECT_SOURCE_DIR}/tests/abi_layout_v1.lp64.txt
        --include ${OCUDU_DAPP_PUBLIC_INCLUDE_DIR}
        --gcc ${OCUDU_DAPP_GCC_EXECUTABLE}
        --gxx ${OCUDU_DAPP_GXX_EXECUTABLE}
        --clang ${OCUDU_DAPP_CLANG_EXECUTABLE}
        --clangxx ${OCUDU_DAPP_CLANGXX_EXECUTABLE})
    set_tests_properties(abi.public_v1_compiler_matrix PROPERTIES TIMEOUT 30)
  elseif(OCUDU_DAPP_REQUIRE_ABI_COMPILER_MATRIX)
    message(FATAL_ERROR
      "The public ABI compiler matrix requires an LP64 target plus gcc, g++, clang, and clang++")
  else()
    message(STATUS "GCC/Clang ABI compiler matrix unavailable; release builds should require it")
  endif()
  add_test(NAME scaffold.class_a_cuda_generation
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/scaffold_class_a_cuda_test.py
      --source ${PROJECT_SOURCE_DIR}
      --binary ${PROJECT_BINARY_DIR}
      --generation-only)
  add_test(NAME scaffold.class_bc_cpu_generation
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/scaffold_class_bc_cpu_test.py
      --source ${PROJECT_SOURCE_DIR}
      --binary ${PROJECT_BINARY_DIR}
      --generation-only)
  # CPU scaffolds build and contract-test end to end on every platform.
  add_test(NAME scaffold.class_bc_cpu
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/scaffold_class_bc_cpu_test.py
      --source ${PROJECT_SOURCE_DIR}
      --binary ${PROJECT_BINARY_DIR}
      --cmake ${CMAKE_COMMAND}
      --sdk-dir ${OCUDUDAppSDK_DIR}
      --contract $<TARGET_FILE:module_contract_test>)
  set_tests_properties(scaffold.class_bc_cpu PROPERTIES TIMEOUT 600)
  add_test(NAME install.public_consumer
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/install_tree_test.py
      --build ${PROJECT_BINARY_DIR}
      --source ${PROJECT_SOURCE_DIR}/tests/install_consumer
      --cmake ${CMAKE_COMMAND}
      --prefix-path "${CMAKE_PREFIX_PATH}"
      --cuda-architectures "${CMAKE_CUDA_ARCHITECTURES}")
  set_tests_properties(install.public_consumer PROPERTIES TIMEOUT 240)
  add_test(NAME docs.release_metadata
    COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tools/check_release_docs.py)
  add_executable(class_c_algorithms_test tests/class_c_algorithms_test.cpp)
  target_link_libraries(class_c_algorithms_test PRIVATE ocudu_reference_class_c_algorithms GTest::gtest_main)
  add_executable(reference_phy_algorithms_test tests/reference_phy_algorithms_test.cpp)
  target_link_libraries(reference_phy_algorithms_test PRIVATE ocudu_reference_phy_algorithms GTest::gtest_main)
  if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
    add_executable(reference_phy_algorithms_cuda_test tests/reference_phy_algorithms_cuda_test.cu)
    target_link_libraries(reference_phy_algorithms_cuda_test PRIVATE
      ocudu_reference_phy_algorithms ref_channel_estimator_cuda_algorithm
      ref_equalizer_cuda_reference_api ref_receiver_cuda_reference_api
      ref_equalizer_cuda_algorithm ref_receiver_cuda_algorithm
      GTest::gtest_main CUDA::cudart)
    set_target_properties(reference_phy_algorithms_cuda_test PROPERTIES
      CUDA_SEPARABLE_COMPILATION ON
      CUDA_RESOLVE_DEVICE_SYMBOLS ON)
    gtest_discover_tests(reference_phy_algorithms_cuda_test)

    add_executable(spectrum_cuda_module_test tests/spectrum_cuda_module_test.cpp)
    target_link_libraries(spectrum_cuda_module_test PRIVATE
      OCUDU::DAppSDK GTest::gtest_main CUDA::cudart ${CMAKE_DL_LIBS})
    add_dependencies(spectrum_cuda_module_test ref_spectrum_cuda)
    add_test(NAME spectrum_cuda.module_contract
      COMMAND ${CMAKE_COMMAND} -E env OCUDU_SPECTRUM_CUDA_MODULE=$<TARGET_FILE:ref_spectrum_cuda>
        $<TARGET_FILE:spectrum_cuda_module_test>)

    add_executable(srs_isac_cuda_module_test tests/srs_isac_cuda_module_test.cpp)
    target_link_libraries(srs_isac_cuda_module_test PRIVATE
      OCUDU::DAppSDK GTest::gtest_main CUDA::cudart ${CMAKE_DL_LIBS})
    add_dependencies(srs_isac_cuda_module_test ref_srs_isac_cuda)
    add_test(NAME srs_isac_cuda.module_contract
      COMMAND ${CMAKE_COMMAND} -E env OCUDU_SRS_ISAC_CUDA_MODULE=$<TARGET_FILE:ref_srs_isac_cuda>
        $<TARGET_FILE:srs_isac_cuda_module_test>)

    add_executable(receiver_cuda_model_management_test tests/receiver_cuda_model_management_test.cpp)
    target_include_directories(receiver_cuda_model_management_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(receiver_cuda_model_management_test PRIVATE
      OCUDU::DAppSDK GTest::gtest_main CUDA::cudart ${CMAKE_DL_LIBS})
    add_dependencies(receiver_cuda_model_management_test ref_receiver_cuda)
    add_test(NAME receiver_cuda.model_management
      COMMAND ${CMAKE_COMMAND} -E env OCUDU_RECEIVER_CUDA_MODULE=$<TARGET_FILE:ref_receiver_cuda>
        $<TARGET_FILE:receiver_cuda_model_management_test>)

    add_executable(receiver_cuda_model_artifact_test tests/receiver_cuda_model_artifact_test.cpp)
    target_include_directories(receiver_cuda_model_artifact_test PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(receiver_cuda_model_artifact_test PRIVATE OCUDU::DAppSDK GTest::gtest_main)
    gtest_discover_tests(receiver_cuda_model_artifact_test)

    add_test(NAME certify.ref_channel_estimator_cuda
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_channel_estimator_cuda> 0x00010003)
    add_test(NAME certify.ref_equalizer_cuda
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_equalizer_cuda> 0x00010004)
    add_test(NAME certify.ref_receiver_cuda
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_receiver_cuda> 0x00010005)

    # L1 A–D suite against open reference packages (shared tool used by vendor repos).
    function(ocudu_add_l1_mode test_name mode)
      add_test(
        NAME ${test_name}
        COMMAND $<TARGET_FILE:l1_validation_suite>
                --mode ${mode}
                --quick
                --eq-so $<TARGET_FILE:ref_equalizer_cuda>
                --rx-so $<TARGET_FILE:ref_receiver_cuda>)
      set_tests_properties(${test_name} PROPERTIES
        LABELS "l1;cuda;class-a;vectors;offline;${mode}"
        TIMEOUT 300
        DEPENDS "ref_equalizer_cuda;ref_receiver_cuda")
    endfunction()
    ocudu_add_l1_mode(l1.golden golden)
    ocudu_add_l1_mode(l1.chain chain)
    ocudu_add_l1_mode(l1.sensitivity sensitivity)
    ocudu_add_l1_mode(l1.tb_crc tb)
    ocudu_add_l1_mode(l1.all all)
    add_test(NAME certify.ref_receiver_cuda_compact
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_receiver_cuda> 0x00010005 --shape-set compact)
    set(CERTIFY_RECEIVER_MODEL ${PROJECT_BINARY_DIR}/certify-receiver-model.bin)
    add_test(NAME certify.receiver_model_artifact
      COMMAND make_receiver_cuda_model ${CERTIFY_RECEIVER_MODEL} 1.0)
    set_tests_properties(certify.receiver_model_artifact PROPERTIES FIXTURES_SETUP receiver_certification_model)
    add_test(NAME certify.ref_receiver_cuda_model
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_receiver_cuda> 0x00010005
        --model-artifact ${CERTIFY_RECEIVER_MODEL}
        --model-id certified-baseline
        --candidate-model-id certified-candidate
        --model-version 1
        --model-generation 1)
    set_tests_properties(certify.ref_receiver_cuda_model PROPERTIES FIXTURES_REQUIRED receiver_certification_model)
    add_test(NAME certify.rejects_wrong_interface
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_channel_estimator_cuda> 0x00010004)
    add_test(NAME certify.rejects_model_on_nonreceiver
      COMMAND certify_class_a_cuda $<TARGET_FILE:ref_channel_estimator_cuda> 0x00010003
        --model-artifact ${CERTIFY_RECEIVER_MODEL}
        --model-id certified-baseline
        --candidate-model-id certified-candidate
        --model-version 1
        --model-generation 1)
    set_tests_properties(certify.rejects_wrong_interface PROPERTIES WILL_FAIL TRUE)
    set_tests_properties(certify.rejects_model_on_nonreceiver PROPERTIES
      WILL_FAIL TRUE FIXTURES_REQUIRED receiver_certification_model)
  endif()
  add_executable(reference_scheduler_algorithm_test tests/reference_scheduler_algorithm_test.cpp)
  target_link_libraries(reference_scheduler_algorithm_test PRIVATE
    ocudu_reference_scheduler_algorithm GTest::gtest_main)
  add_executable(reference_scheduler_pipeline_test tests/reference_scheduler_pipeline_test.cpp)
  target_link_libraries(reference_scheduler_pipeline_test PRIVATE
    ocudu_reference_scheduler_algorithm GTest::gtest_main)
  include(GoogleTest)
  gtest_discover_tests(class_c_algorithms_test)
  gtest_discover_tests(reference_phy_algorithms_test)
  gtest_discover_tests(reference_scheduler_algorithm_test)
  gtest_discover_tests(reference_scheduler_pipeline_test)
  add_executable(module_contract_test tests/module_contract_test.cpp)
  target_link_libraries(module_contract_test PRIVATE OCUDU::DAppSDK ${CMAKE_DL_LIBS})

  add_executable(package_config_test tests/package_config_test.cpp)
  target_link_libraries(package_config_test PRIVATE ocudu_reference_support GTest::gtest_main)
  gtest_discover_tests(package_config_test)

  if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
    add_test(NAME certify.rejects_cuda_output_overrun
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/certifier_rejection_test.py
        $<TARGET_FILE:certify_class_a_cuda> $<TARGET_FILE:certifier_bad_bounds_cuda>
        "wrote outside its advertised byte range")
    add_test(NAME certify.rejects_cuda_storage_substitution
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/certifier_rejection_test.py
        $<TARGET_FILE:certify_class_a_cuda> $<TARGET_FILE:certifier_bad_storage_cuda>
        "substituted caller-owned output storage")
    add_test(NAME certify.rejects_more_than_two_layers
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/certifier_rejection_test.py
        $<TARGET_FILE:certify_class_a_cuda> $<TARGET_FILE:certifier_bad_layers_cuda>
        "declared profile is outside the reference v1 envelope")
    add_test(NAME certify.rejects_transform_precoding
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/certifier_rejection_test.py
        $<TARGET_FILE:certify_class_a_cuda> $<TARGET_FILE:certifier_bad_transform_cuda>
        "declared profile is outside the reference v1 envelope")
    add_test(NAME scaffold.class_a_cuda_build
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/scaffold_class_a_cuda_test.py
        --source ${PROJECT_SOURCE_DIR}
        --binary ${PROJECT_BINARY_DIR}
        --build-only
        --cmake ${CMAKE_COMMAND}
        --sdk-dir ${OCUDUDAppSDK_DIR}
        --contract $<TARGET_FILE:module_contract_test>
        --cuda-architectures "${CMAKE_CUDA_ARCHITECTURES}")
    add_test(NAME scaffold.class_a_cuda
      COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/scaffold_class_a_cuda_test.py
        --source ${PROJECT_SOURCE_DIR}
        --binary ${PROJECT_BINARY_DIR}
        --cmake ${CMAKE_COMMAND}
        --sdk-dir ${OCUDUDAppSDK_DIR}
        --contract $<TARGET_FILE:module_contract_test>
        --certifier $<TARGET_FILE:certify_class_a_cuda>
        --cuda-architectures "${CMAKE_CUDA_ARCHITECTURES}")
    set_tests_properties(scaffold.class_a_cuda_build scaffold.class_a_cuda PROPERTIES TIMEOUT 180)
  endif()
  add_executable(module_state_test tests/module_state_test.cpp)
  target_link_libraries(module_state_test PRIVATE ocudu_reference_support GTest::gtest_main)
  gtest_discover_tests(module_state_test)
  add_executable(process_client_test tests/process/process_client_test.cpp)
  target_link_libraries(process_client_test PRIVATE
    ocudu_process_client ocudu_reference_class_c_input ocudu_reference_class_c_algorithms ocudu_reference_support)
  add_test(NAME process_client.route_interoperability COMMAND process_client_test)
  add_test(NAME process_client.binds_parent_lifetime COMMAND process_client_test --parent-lifetime)
  add_test(NAME process_client.enforces_parent_death COMMAND process_client_test --parent-death-enforced)
  add_test(NAME process_client.sharded_route_set COMMAND process_client_test --route-set)
  add_test(NAME process_client.multi_interface_route_bundle COMMAND process_client_test --route-bundle)
  add_test(NAME process_client.authenticated_heartbeat COMMAND process_client_test --heartbeat-route-set)
  add_test(NAME process_client.authenticated_management COMMAND process_client_test --management-route-set)
  add_test(NAME process_client.rejects_duplicate_shard COMMAND process_client_test --duplicate-route-set)
  add_test(NAME process_client.rejects_incomplete_shard_set COMMAND process_client_test --incomplete-route-set)
  add_test(NAME process_client.decodes_complex_bf16 COMMAND process_client_test --decode-cbf16)
  add_test(NAME process_client.decodes_portable_host COMMAND process_client_test --decode-portable-host)
  if(OCUDU_DAPP_REF_EXTRAS AND OCUDUDAppSDK_E3_MANAGEMENT_AVAILABLE)
    foreach(interface IN ITEMS spectrum srs-isac)
      string(REPLACE "-" "_" target_interface ${interface})
      add_test(NAME portable_e3.${target_interface}
        COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/portable_e3_consumer_test.py
          --consumer $<TARGET_FILE:ref_${target_interface}_e3_cpu>
          --schema ${OCUDUDAppSDK_E3_MANAGEMENT_SCHEMA}
          --class-c-schema ${OCUDUDAppSDK_CLASS_C_SCHEMA}
          --flatc ${FLATC_EXECUTABLE}
          --interface ${interface})
      set_tests_properties(portable_e3.${target_interface} PROPERTIES TIMEOUT 20)
    endforeach()
  elseif(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES AND OCUDU_DAPP_REF_EXTRAS)
    message(FATAL_ERROR
      "portable_e3 consumer round-trip tests deregistered: E3 management schema unavailable "
      "(OCUDU_DAPP_REQUIRE_PYTHON_EXAMPLES=ON)")
  endif()
  add_test(NAME process_client.decodes_advance_ul_plan COMMAND process_client_test --decode-ul-plan)
  add_test(NAME process_client.restricted_policy COMMAND process_client_test --restricted-policy)
  if(OCUDU_DAPP_REF_EXTRAS)
    add_test(NAME process_worker.spectrum
      COMMAND process_client_test $<TARGET_FILE:ref_spectrumd_cpu> 0x00010001)
    add_test(NAME process_worker.spectrum_sharded
      COMMAND process_client_test $<TARGET_FILE:ref_spectrumd_cpu> 0x00010001 0 --sharded)
    add_test(NAME process_worker.spectrum_observer
      COMMAND process_client_test $<TARGET_FILE:ref_spectrum_observerd_cpu> 0x00010001 --observer)
    add_test(NAME process_worker.srs_isac
      COMMAND process_client_test $<TARGET_FILE:ref_srs_isacd_cpu> 0x00010002)
    add_test(NAME process_worker.srs_optional_dmrs_bundle
      COMMAND process_client_test --bundle-worker $<TARGET_FILE:test_srs_dmrs_fusiond_cpu>)
    add_test(NAME process_worker.rejects_wrong_interface
      COMMAND process_client_test $<TARGET_FILE:ref_spectrumd_cpu> 0x00010002 3)
    set_tests_properties(process_worker.spectrum process_worker.spectrum_sharded process_worker.spectrum_observer
      process_worker.srs_isac process_worker.srs_optional_dmrs_bundle process_worker.rejects_wrong_interface
      PROPERTIES TIMEOUT 5)
  endif()
endif()

# ---------------------------------------------------------------------------
# Examples are part of the shipped product, not the test suite: they build and
# install regardless of BUILD_TESTING (each guards its own add_test calls).
if(OCUDU_DAPP_BUILD_CUDA_EXAMPLES)
  add_subdirectory(examples/vendor_min)
endif()
if(OCUDU_DAPP_BUILD_PYTHON_CLASS_C)
  add_subdirectory(examples/class_c_e3_python)
endif()
add_subdirectory(examples/e3_telemetry_python)
add_subdirectory(examples/e3_receiver_routing)
add_subdirectory(examples/e3_spectrum_sensing)
add_subdirectory(examples/e3_isac_sensing)
add_subdirectory(examples/e3_mcp_server)

# ---------------------------------------------------------------------------
# Single install package for ocudu-dapp-sdk: find_package(OCUDUDapp).
# Host ABI is a dependency (find_dependency(OCUDUDAppSDK) from ocudu install).
# ---------------------------------------------------------------------------
option(OCUDU_DAPP_INSTALL_VENDOR_TOOLKIT
  "Install certifier, L1 suite, module_support, and open Class-A CUDA libs for vendor packages"
  ON)
if(OCUDU_DAPP_INSTALL_VENDOR_TOOLKIT)
  set(OCUDU_DAPP_PKG_CMAKE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/OCUDUDapp)

  install(DIRECTORY ${PROJECT_SOURCE_DIR}/include/ocudu_dapp_sdk
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h" PATTERN "*.cuh")
  install(FILES ${CLASS_C_GENERATED_HEADER}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/ocudu_dapp_sdk/generated)
  install(FILES
      ${PROJECT_SOURCE_DIR}/README.md
      ${PROJECT_SOURCE_DIR}/LICENSE
      ${PROJECT_SOURCE_DIR}/REUSE.toml
      ${PROJECT_SOURCE_DIR}/CHANGELOG.md
      ${PROJECT_SOURCE_DIR}/CONTRIBUTING.md
      ${PROJECT_SOURCE_DIR}/SECURITY.md
      ${PROJECT_SOURCE_DIR}/CODE_OF_CONDUCT.md
      ${PROJECT_SOURCE_DIR}/SUPPORT.md
      ${PROJECT_SOURCE_DIR}/API_STABILITY.md
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/LICENSES/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/LICENSES)
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/docs
    FILES_MATCHING PATTERN "*.md")
  install(DIRECTORY
      ${PROJECT_SOURCE_DIR}/examples/vendor_min
      ${PROJECT_SOURCE_DIR}/examples/class_c_e3_python
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples
    FILES_MATCHING PATTERN "*.md")
  install(DIRECTORY ${PROJECT_SOURCE_DIR}/tools/l1_validation/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/tools/l1_validation
    FILES_MATCHING PATTERN "*.md")
  if(TARGET certify_class_a_cuda)
    install(TARGETS certify_class_a_cuda l1_validation_suite
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  # Coherent public CPU authoring surface. Every installed header that needs a
  # compiled implementation has a matching exported target.
  install(TARGETS
      ocudu_reference_class_c_types
      ocudu_reference_support
      ocudu_reference_class_c_algorithms
      ocudu_reference_phy_algorithms
      ocudu_reference_scheduler_algorithm
      ocudu_process_client
      ocudu_reference_class_c_input
    EXPORT OCUDUDappTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  # Portable E3 consumer (PIC) for out-of-tree clients / pybind rebuilds.
  # Requires host E3 statics with POSITION_INDEPENDENT_CODE. The generated
  # FlatBuffers type and decoder dependencies are part of the exported surface.
  set(OCUDU_DAPP_TOOLKIT_PORTABLE_E3_AVAILABLE FALSE)
  if(TARGET ocudu_portable_e3_consumer)
    set(OCUDU_DAPP_TOOLKIT_PORTABLE_E3_AVAILABLE TRUE)
    install(TARGETS ocudu_portable_e3_consumer
      EXPORT OCUDUDappTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
  endif()

  # Native Python extensions (optional build artifacts).
  if(TARGET ocudu_dapp_class_c_e3)
    install(TARGETS ocudu_dapp_class_c_e3
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/python
      RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/python)
  endif()
  if(TARGET ocudu_dapp_class_c_process)
    install(TARGETS ocudu_dapp_class_c_process
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/python
      RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/python)
  endif()
  if(TARGET class_c_python_spectrumd)
    install(TARGETS class_c_python_spectrumd
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(TARGET class_c_python_srs_isacd)
    install(TARGETS class_c_python_srs_isacd
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()
  if(TARGET ocudu_dapp_class_c_e3 OR TARGET ocudu_dapp_class_c_process)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/examples/class_c_e3_python/sensing
      DESTINATION ${CMAKE_INSTALL_DATADIR}/ocudu_dapp_sdk_examples/class_c_e3_python
      FILES_MATCHING PATTERN "*.py"
      PATTERN "__pycache__" EXCLUDE)
  endif()

  set(OCUDU_DAPP_TOOLKIT_CUDA_AVAILABLE FALSE)
  if(TARGET ref_equalizer_cuda_algorithm)
    set(OCUDU_DAPP_TOOLKIT_CUDA_AVAILABLE TRUE)
    install(TARGETS
        ref_channel_estimator_cuda_algorithm
        ref_equalizer_cuda_algorithm
        ref_receiver_cuda_algorithm
        ref_spectrum_cuda_algorithm
        ref_srs_isac_cuda_algorithm
        ocudu_process_cuda_client
      EXPORT OCUDUDappTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
      INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS make_receiver_cuda_model
      RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endif()

  install(PROGRAMS ${PROJECT_SOURCE_DIR}/tools/scaffold_class_a_cuda.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME ocudu-dapp-scaffold-class-a-cuda)
  install(PROGRAMS ${PROJECT_SOURCE_DIR}/tools/scaffold_class_bc_cpu.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME ocudu-dapp-scaffold-class-bc-cpu)

  include(CMakePackageConfigHelpers)
  configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/OCUDUDappConfig.cmake.in
    ${PROJECT_BINARY_DIR}/OCUDUDappConfig.cmake
    INSTALL_DESTINATION ${OCUDU_DAPP_PKG_CMAKE_DIR})
  write_basic_package_version_file(
    ${PROJECT_BINARY_DIR}/OCUDUDappConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMinorVersion)
  install(EXPORT OCUDUDappTargets
    FILE OCUDUDappTargets.cmake
    NAMESPACE OCUDUDapp::
    DESTINATION ${OCUDU_DAPP_PKG_CMAKE_DIR})
  install(FILES
      ${PROJECT_BINARY_DIR}/OCUDUDappConfig.cmake
      ${PROJECT_BINARY_DIR}/OCUDUDappConfigVersion.cmake
    DESTINATION ${OCUDU_DAPP_PKG_CMAKE_DIR})
endif()

# Optional OCUDU-style HTML documentation (requires Doxygen).
add_subdirectory(docs/doxygen)
