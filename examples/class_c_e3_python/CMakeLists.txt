# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Python Class-C out-of-process dApps (research / education example):
#   - Portable E3 (sampled): ocudu_dapp_class_c_e3
#   - Supervised SHMEM/CUDA-IPC + DLPack catalog packages:
#       ocudu_dapp_class_c_process
#       class_c_python_spectrumd   → spectrum_worker.py  (radiometer)
#       class_c_python_srs_isacd   → srs_isac_worker.py  (CIR estimate)
#
# Python is NOT supported for Class-A/B or in-process Class-C MODULEs.
# Prefer ref_spectrumd_* / ref_srs_isacd_* for production performance.

find_package(Python3 COMPONENTS Interpreter Development QUIET)
if(NOT Python3_Interpreter_FOUND)
  message(STATUS "class_c_e3_python: Python3 not found — skipped")
  return()
endif()

set(_ocudu_have_torch FALSE)
execute_process(
  COMMAND "${Python3_EXECUTABLE}" -c "import torch"
  RESULT_VARIABLE _torch_rc
  OUTPUT_QUIET ERROR_QUIET)
if(_torch_rc EQUAL 0)
  set(_ocudu_have_torch TRUE)
endif()

if(BUILD_TESTING AND _ocudu_have_torch)
  add_test(NAME example.class_c_e3_python.offline_sensing
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_spectrum_sensing.py -v)
  set_tests_properties(example.class_c_e3_python.offline_sensing PROPERTIES
    LABELS "example;class-c;python;offline" TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME example.class_c_e3_python.offline_srs_cir
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_srs_cir.py -v)
  set_tests_properties(example.class_c_e3_python.offline_srs_cir PROPERTIES
    LABELS "example;class-c;python;offline;srs" TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})

  add_test(NAME example.class_c_e3_python.offline_app
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/sensing/app.py
            --mode offline --device cpu --prbs 12)
  set_tests_properties(example.class_c_e3_python.offline_app PROPERTIES
    LABELS "example;class-c;python;offline" TIMEOUT 60
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
elseif(BUILD_TESTING AND NOT _ocudu_have_torch)
  message(STATUS "class_c_e3_python: PyTorch not importable — offline tests not registered")
endif()

if(NOT Python3_Development_FOUND)
  find_package(Python3 COMPONENTS Interpreter Development QUIET)
endif()
find_package(pybind11 CONFIG QUIET)
if(NOT pybind11_FOUND)
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE _pybind11_cmake_dir
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _pybind11_rc
    ERROR_QUIET)
  if(_pybind11_rc EQUAL 0 AND IS_DIRECTORY "${_pybind11_cmake_dir}")
    list(APPEND CMAKE_PREFIX_PATH "${_pybind11_cmake_dir}")
    find_package(pybind11 CONFIG QUIET)
  endif()
endif()
if(NOT pybind11_FOUND OR NOT Python3_Development_FOUND)
  message(STATUS "class_c_e3_python: pybind11/Python dev missing — native extensions skipped")
  return()
endif()

# --- Portable E3 ---
if(TARGET ocudu_portable_e3_consumer)
  pybind11_add_module(ocudu_dapp_class_c_e3 MODULE bindings/module.cpp)
  target_link_libraries(ocudu_dapp_class_c_e3 PRIVATE
    ocudu_portable_e3_consumer ocudu_reference_class_c_input OCUDU::DAppSDK)
  if(TARGET OCUDU::DAppManagementClient)
    target_link_libraries(ocudu_dapp_class_c_e3 PRIVATE OCUDU::DAppManagementClient)
  endif()
  target_include_directories(ocudu_dapp_class_c_e3 PRIVATE ${PROJECT_SOURCE_DIR}/include)
  set_target_properties(ocudu_dapp_class_c_e3 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    POSITION_INDEPENDENT_CODE ON)
  if(BUILD_TESTING)
    add_test(NAME example.class_c_e3_python.import_extension
      COMMAND ${Python3_EXECUTABLE} -c
              "import ocudu_dapp_class_c_e3 as m; assert m.INTERFACE_SPECTRUM == 0x00010001")
    set_tests_properties(example.class_c_e3_python.import_extension PROPERTIES
      LABELS "example;class-c;python;e3" TIMEOUT 30
      ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}" DEPENDS ocudu_dapp_class_c_e3)
  endif()
endif()

# --- Process SHMEM / CUDA-IPC + two dApp launchers ---
if(TARGET ocudu_process_client AND TARGET ocudu_reference_class_c_input AND TARGET ocudu_reference_support)
  set(_py_proc_libs ocudu_process_client ocudu_reference_class_c_input ocudu_reference_support OCUDU::DAppSDK)
  set(_py_proc_defs "")
  if(TARGET ocudu_process_cuda_client)
    list(APPEND _py_proc_libs ocudu_process_cuda_client CUDA::cudart)
    list(APPEND _py_proc_defs OCUDU_DAPP_PY_CUDA_PROCESS=1)
    set_target_properties(ocudu_process_cuda_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
  endif()

  pybind11_add_module(ocudu_dapp_class_c_process MODULE bindings/process_module.cpp)
  target_link_libraries(ocudu_dapp_class_c_process PRIVATE ${_py_proc_libs})
  target_include_directories(ocudu_dapp_class_c_process PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/bindings)
  target_compile_definitions(ocudu_dapp_class_c_process PRIVATE ${_py_proc_defs})
  set_target_properties(ocudu_dapp_class_c_process PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    POSITION_INDEPENDENT_CODE ON)

  set(_spectrum_py ${CMAKE_CURRENT_SOURCE_DIR}/sensing/spectrum_worker.py)
  set(_srs_py ${CMAKE_CURRENT_SOURCE_DIR}/sensing/srs_isac_worker.py)

  # DU posix_spawn strips the environment to PATH/LANG. Bake absolute interpreter,
  # PYTHONPATH (extension .so), LD_LIBRARY_PATH (torch/cuda), and default log files
  # so supervised workers are self-contained after catalog staging.
  set(_py_exe "${Python3_EXECUTABLE}")
  set(_py_path "${CMAKE_BINARY_DIR}")
  set(_py_ld "")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c
      "import os, pathlib, torch; t=pathlib.Path(torch.__file__).parent; libs=[str(t/'lib')]; site=t.parent; libs += [str(p) for p in site.glob('nvidia/*/lib')]; cuda=os.environ.get('CUDA_HOME','/usr/local/cuda'); libs += [f'{cuda}/lib64', f'{cuda}/targets/sbsa-linux/lib']; print(':'.join(libs))"
    OUTPUT_VARIABLE _py_ld
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
  if(NOT _py_ld)
    set(_py_ld "/usr/local/cuda/lib64")
  endif()

  # Default JSONL under the build tree. Lab/OTA trees can bake absolute paths:
  #   -DOCUDU_DAPP_PYTHON_SPECTRUM_LOG=/path/runtime_logs/python_spectrum.jsonl
  #   -DOCUDU_DAPP_PYTHON_SRS_LOG=/path/runtime_logs/python_srs_isac.jsonl
  # Runtime override (when the spawn env is not stripped): OCUDU_DAPP_LOG_FILE.
  set(OCUDU_DAPP_PYTHON_SPECTRUM_LOG
    "${CMAKE_BINARY_DIR}/runtime_logs/python_spectrum.jsonl"
    CACHE FILEPATH "Default --log-file for class_c_python_spectrumd")
  set(OCUDU_DAPP_PYTHON_SRS_LOG
    "${CMAKE_BINARY_DIR}/runtime_logs/python_srs_isac.jsonl"
    CACHE FILEPATH "Default --log-file for class_c_python_srs_isacd")
  set(_spectrum_log "${OCUDU_DAPP_PYTHON_SPECTRUM_LOG}")
  set(_srs_log "${OCUDU_DAPP_PYTHON_SRS_LOG}")
  get_filename_component(_log_parent "${_spectrum_log}" DIRECTORY)
  if(_log_parent)
    file(MAKE_DIRECTORY "${_log_parent}")
  endif()


  function(ocudu_class_c_python_launcher target worker_py log_file)
    add_executable(${target} launcher/class_c_python_launcher.cpp)
    target_compile_definitions(${target} PRIVATE
      "OCUDU_DAPP_PYTHON_PROCESS_WORKER_DEFAULT=\"${worker_py}\""
      "OCUDU_DAPP_PYTHON_DEFAULT=\"${_py_exe}\""
      "OCUDU_DAPP_PYTHONPATH_DEFAULT=\"${_py_path}\""
      "OCUDU_DAPP_LD_LIBRARY_PATH_DEFAULT=\"${_py_ld}\""
      "OCUDU_DAPP_LOG_FILE_DEFAULT=\"${log_file}\"")
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
  endfunction()

  ocudu_class_c_python_launcher(class_c_python_spectrumd "${_spectrum_py}" "${_spectrum_log}")
  ocudu_class_c_python_launcher(class_c_python_srs_isacd "${_srs_py}" "${_srs_log}")

  # Catalog manifests (out_of_process Class-C).
  if(EXISTS ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake)
    function(add_python_process_manifest target package_id global_name interface_id capabilities backend)
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
          "-DVENDOR=OCUDU example"
          -DVERSION=1
          -DDAPP_CLASS=C
          -DINTERFACE_ID=${interface_id}
          -DPACKAGE_PLACEMENT=out_of_process
          -DINTERFACE_PLACEMENT=out_of_process
          -DCAPABILITIES=${capabilities}
          -DBACKEND=${backend}
          -P ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
        DEPENDS ${target} ${PROJECT_SOURCE_DIR}/cmake/write_package_manifest.cmake
        VERBATIM)
      add_custom_target(${target}_package ALL DEPENDS ${manifest} ${sbom})
    endfunction()

    # Spectrum capabilities 7 = map + quiet bits (match ref_spectrumd_cpu); SRS 0 = preview only.
    add_python_process_manifest(class_c_python_spectrumd
      8f0d0001-4f9c-4e31-8c00-00000000e101
      org.ocudu.example.python.spectrum.process
      0x00010001 7 cpu)
    add_python_process_manifest(class_c_python_srs_isacd
      8f0d0001-4f9c-4e31-8c00-00000000e102
      org.ocudu.example.python.srs-isac.process
      0x00010002 0 cpu)
  endif()

  if(BUILD_TESTING)
    add_test(NAME example.class_c_python.import_process
      COMMAND ${Python3_EXECUTABLE} -c
              "import ocudu_dapp_class_c_process as m; assert hasattr(m.ProcessWorker, 'send_srs_cir_result')")
    set_tests_properties(example.class_c_python.import_process PROPERTIES
      LABELS "example;class-c;python;process" TIMEOUT 30
      ENVIRONMENT "PYTHONPATH=${CMAKE_BINARY_DIR}" DEPENDS ocudu_dapp_class_c_process)
  endif()
endif()
