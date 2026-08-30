# SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
# SPDX-License-Identifier: BSD-3-Clause-Clear

if(NOT DEFINED MANIFEST OR NOT DEFINED SBOM OR NOT DEFINED ARTIFACT OR NOT DEFINED EXPECTED_CAPABILITIES OR
    NOT EXISTS "${MANIFEST}" OR NOT EXISTS "${SBOM}" OR NOT EXISTS "${ARTIFACT}")
  message(FATAL_ERROR "Manifest, SBOM, artifact and expected capabilities are required")
endif()

file(READ "${MANIFEST}" document LIMIT 262145)
string(LENGTH "${document}" document_size)
if(document_size GREATER 262144)
  message(FATAL_ERROR "Manifest exceeds the admission bound")
endif()
execute_process(COMMAND /usr/bin/stat -c %a "${ARTIFACT}"
  RESULT_VARIABLE artifact_mode_result OUTPUT_VARIABLE artifact_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND /usr/bin/stat -c %a "${MANIFEST}"
  RESULT_VARIABLE manifest_mode_result OUTPUT_VARIABLE manifest_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND /usr/bin/stat -c %a "${SBOM}"
  RESULT_VARIABLE sbom_mode_result OUTPUT_VARIABLE sbom_mode OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT artifact_mode_result EQUAL 0 OR NOT artifact_mode STREQUAL "555")
  message(FATAL_ERROR "Packaged artifact must have mode 0555")
endif()
if(NOT manifest_mode_result EQUAL 0 OR NOT manifest_mode STREQUAL "444")
  message(FATAL_ERROR "Package manifest must have mode 0444")
endif()
if(NOT sbom_mode_result EQUAL 0 OR NOT sbom_mode STREQUAL "444")
  message(FATAL_ERROR "Package SBOM must have mode 0444")
endif()
file(SHA256 "${ARTIFACT}" digest)
file(SHA256 "${SBOM}" sbom_digest)
file(SHA1 "${ARTIFACT}" artifact_sha1)
get_filename_component(artifact_name "${ARTIFACT}" NAME)
get_filename_component(sbom_name "${SBOM}" NAME)
foreach(fragment IN ITEMS
    "\"schema_version\": 1"
    "\"artifact\": \"${artifact_name}\""
    "\"sha256\": \"${digest}\""
    "\"capabilities\": ${EXPECTED_CAPABILITIES}"
    "\"interfaces\": ["
    "\"provenance\": {"
    "\"sbom\": \"${sbom_name}\""
    "\"format\": \"spdx-2.3-json\""
    "\"sha256\": \"${sbom_digest}\"")
  string(FIND "${document}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Generated manifest is missing: ${fragment}")
  endif()
endforeach()

if(DEFINED EXPECTED_INTERFACE_IDS)
  foreach(interface_id IN LISTS EXPECTED_INTERFACE_IDS)
    math(EXPR interface_id_decimal "${interface_id}")
    string(FIND "${document}" "\"interface_id\": ${interface_id_decimal}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "Generated manifest is missing interface ${interface_id}")
    endif()
  endforeach()
endif()
if(DEFINED EXPECTED_INTERFACE_COUNT)
  string(REGEX MATCHALL "\"interface_id\"[ \t]*:" interface_matches "${document}")
  list(LENGTH interface_matches interface_count)
  if(NOT interface_count EQUAL EXPECTED_INTERFACE_COUNT)
    message(FATAL_ERROR "Generated manifest has ${interface_count} interfaces; expected ${EXPECTED_INTERFACE_COUNT}")
  endif()
endif()
if(DEFINED EXPECTED_BINDINGS)
  foreach(binding IN LISTS EXPECTED_BINDINGS)
    if(NOT binding MATCHES "^(required|optional)$")
      message(FATAL_ERROR "Invalid expected binding policy: ${binding}")
    endif()
    string(FIND "${document}" "\"binding\": \"${binding}\"" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "Generated manifest is missing binding policy ${binding}")
    endif()
  endforeach()
endif()

file(READ "${SBOM}" sbom LIMIT 4194305)
string(LENGTH "${sbom}" sbom_size)
if(sbom_size GREATER 4194304)
  message(FATAL_ERROR "Package SBOM exceeds the admission bound")
endif()
foreach(fragment IN ITEMS
    "\"spdxVersion\": \"SPDX-2.3\""
    "\"dataLicense\": \"CC0-1.0\""
    "\"SPDXID\": \"SPDXRef-DOCUMENT\""
    "\"SPDXID\": \"SPDXRef-Package\""
    "\"SPDXID\": \"SPDXRef-Artifact\""
    "\"fileName\": \"./${artifact_name}\""
    "\"algorithm\": \"SHA1\", \"checksumValue\": \"${artifact_sha1}\""
    "\"algorithm\": \"SHA256\", \"checksumValue\": \"${digest}\""
    "\"relationshipType\": \"DESCRIBES\"")
  string(FIND "${sbom}" "${fragment}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Generated SPDX SBOM is missing: ${fragment}")
  endif()
endforeach()

string(FIND "${document}" "\"class\": \"A\"" class_a_position)
if(NOT class_a_position EQUAL -1)
  string(FIND "${document}" "\"backends\": [\"cuda\"]" cuda_position)
  if(cuda_position EQUAL -1)
    set(expected_layers 1)
    set(expected_flags 0)
  else()
    set(expected_layers 2)
    set(expected_flags 2)
  endif()
  foreach(fragment IN ITEMS
      "\"max_prb\": 275"
      "\"max_rx_ports\": 64"
      "\"max_layers\": ${expected_layers}"
      "\"modulation_mask\": 15"
      "\"dmrs_type_mask\": 3"
      "\"flags\": ${expected_flags}")
    string(FIND "${document}" "${fragment}" position)
    if(position EQUAL -1)
      message(FATAL_ERROR "Generated Class-A manifest violates the reference v1 envelope: ${fragment}")
    endif()
  endforeach()
endif()
