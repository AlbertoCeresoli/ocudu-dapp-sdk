# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

cmake_minimum_required(VERSION 3.20)
project(ocudu_dapp_install_consumer LANGUAGES CXX)

find_package(OCUDUDapp 0.1 CONFIG REQUIRED)

if(NOT OCUDUDapp_ABI_MAJOR EQUAL 1)
  message(FATAL_ERROR "Expected OCUDU dApp ABI major 1")
endif()

add_executable(ocudu_dapp_cpu_consumer main.cpp)
target_compile_features(ocudu_dapp_cpu_consumer PRIVATE cxx_std_17)
target_link_libraries(ocudu_dapp_cpu_consumer PRIVATE
  OCUDUDapp::DAppSDK
  OCUDUDapp::reference_support
  OCUDUDapp::reference_class_c_algorithms
  OCUDUDapp::reference_phy_algorithms
  OCUDUDapp::reference_srs_channel_estimator
  OCUDUDapp::reference_scheduler_algorithm
  OCUDUDapp::process_client
  OCUDUDapp::reference_class_c_input)

if(OCUDUDapp_CUDA_AVAILABLE)
  enable_language(CUDA)
  find_package(CUDAToolkit REQUIRED)
  add_executable(ocudu_dapp_cuda_consumer cuda_consumer.cu)
  set_target_properties(ocudu_dapp_cuda_consumer PROPERTIES
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
    CUDA_SEPARABLE_COMPILATION ON)
  target_link_libraries(ocudu_dapp_cuda_consumer PRIVATE
    OCUDUDapp::ref_channel_estimator_cuda_algorithm
    OCUDUDapp::ref_equalizer_cuda_algorithm
    OCUDUDapp::ref_receiver_cuda_algorithm
    OCUDUDapp::ref_spectrum_cuda_algorithm
    OCUDUDapp::ref_srs_isac_cuda_algorithm
    OCUDUDapp::ref_srs_channel_estimator_cuda_algorithm
    OCUDUDapp::process_cuda_client)
endif()
