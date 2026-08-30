// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include <ocudu/dapp/abi/v1/module.h>
#include <ocudu/dapp/process/v1/control.h>
#include <ocudu/dapp/process/v1/cuda_export.h>
#include <ocudu/dapp/process/v1/publication_record.h>
#include <ocudu/dapp/use_cases/v1/channel_estimator.h>
#include <ocudu/dapp/use_cases/v1/coordination.h>
#include <ocudu/dapp/use_cases/v1/equalizer.h>
#include <ocudu/dapp/use_cases/v1/receiver.h>
#include <ocudu/dapp/use_cases/v1/scheduler.h>
#include <ocudu/dapp/use_cases/v1/spectrum.h>
#include <ocudu/dapp/use_cases/v1/srs_isac.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
#define OCUDU_ALIGNOF(type) alignof(type)
#else
#define OCUDU_ALIGNOF(type) _Alignof(type)
#endif

#define PRINT_TYPE(type) printf("type %s %zu %zu\n", #type, sizeof(type), (size_t)OCUDU_ALIGNOF(type))
#define PRINT_FIELD(type, field) printf("field %s.%s %zu\n", #type, #field, offsetof(type, field))
#define PRINT_VALUE(value) printf("value %s %llu\n", #value, (unsigned long long)(value))

int main(void)
{
  PRINT_VALUE(OCUDU_DAPP_ABI_MAJOR_V1);
  PRINT_VALUE(OCUDU_DAPP_ABI_MINOR_V1);
  PRINT_VALUE(OCUDU_DAPP_PROCESS_REQUIRED_CAPABILITIES_V1);
  PRINT_VALUE(OCUDU_DAPP_PROCESS_CAP_CUDA_IPC_V1);
  PRINT_VALUE(OCUDU_DAPP_PROCESS_CAP_MULTI_INTERFACE_V1);
  PRINT_VALUE(OCUDU_DAPP_PROCESS_MAX_INTERFACES_V1);
  PRINT_VALUE(OCUDU_DAPP_ROUTE_DESCRIPTOR_ACCELERATOR_EXPORT_V1);
  PRINT_VALUE(OCUDU_DAPP_ROUTE_DESCRIPTOR_CUDA_COUNT_V1);
  PRINT_VALUE(OCUDU_DAPP_ROUTE_TRANSPORT_CPU_SHARED_RING_V1);
  PRINT_VALUE(OCUDU_DAPP_ROUTE_TRANSPORT_CUDA_IPC_EXPORT_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_SPECTRUM_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_SRS_ISAC_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_CHANNEL_ESTIMATOR_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_EQUALIZER_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_RECEIVER_V1);
  PRINT_VALUE(OCUDU_DAPP_METADATA_RECEIVER_METRICS_V1);
  PRINT_VALUE(OCUDU_DAPP_RECEIVER_METRICS_VALID_V1);
  PRINT_VALUE(OCUDU_DAPP_INTERFACE_SCHEDULER_V1);
  PRINT_VALUE(OCUDU_DAPP_SCHEDULER_CANDIDATE_BASE_SIZE_V1);
  PRINT_VALUE(OCUDU_DAPP_SCHEDULER_CANDIDATE_BLER_COUNT_SIZE_V1);
  PRINT_VALUE(OCUDU_DAPP_SCHEDULER_BLER_WINDOW_SIZE_V1);
  PRINT_VALUE(OCUDU_DAPP_PUSCH_CAP_RESIDENT_ACCELERATOR_V1);
  PRINT_VALUE(OCUDU_DAPP_LAYOUT_PUSCH_CHANNEL_PORT_LAYER_SYMBOL_SUBCARRIER_V1);
  PRINT_VALUE(OCUDU_DAPP_LAYOUT_PUSCH_EQUALIZED_DATA_RE_LAYER_V1);
  PRINT_VALUE(OCUDU_DAPP_LAYOUT_PUSCH_POST_EQ_NOISE_DATA_RE_LAYER_V1);
  PRINT_VALUE(OCUDU_DAPP_LAYOUT_PUSCH_LLR_DATA_RE_LAYER_BIT_V1);
  PRINT_VALUE(OCUDU_DAPP_LAYOUT_SRS_CHANNEL_RX_TX_FREQUENCY_CF32_V1);

  PRINT_TYPE(ocudu_dapp_abi_header_v1);
  PRINT_TYPE(ocudu_dapp_blob_view_v1);
  PRINT_TYPE(ocudu_dapp_radio_time_v1);
  PRINT_TYPE(ocudu_dapp_tensor_view_v1);
  PRINT_TYPE(ocudu_dapp_invocation_v1);
  PRINT_TYPE(ocudu_dapp_context_header_v1);
  PRINT_TYPE(ocudu_dapp_host_api_v1);
  PRINT_TYPE(ocudu_dapp_worker_api_v1);
  PRINT_TYPE(ocudu_dapp_interface_descriptor_v1);
  PRINT_TYPE(ocudu_dapp_module_v1);
  PRINT_TYPE(ocudu_dapp_host_info_v1);
  PRINT_TYPE(ocudu_dapp_value_v1);
  PRINT_TYPE(ocudu_dapp_config_descriptor_v1);
  PRINT_TYPE(ocudu_dapp_config_entry_v1);
  PRINT_TYPE(ocudu_dapp_config_update_v1);
  PRINT_TYPE(ocudu_dapp_metric_descriptor_v1);
  PRINT_TYPE(ocudu_dapp_metric_sample_v1);
  PRINT_TYPE(ocudu_dapp_model_request_v1);
  PRINT_TYPE(ocudu_dapp_management_v1);
  PRINT_TYPE(ocudu_dapp_typed_metadata_v1);
  PRINT_TYPE(ocudu_dapp_pusch_profile_v1);
  PRINT_TYPE(ocudu_dapp_pusch_metadata_v1);
  PRINT_TYPE(ocudu_dapp_dmrs_metadata_v1);
  PRINT_TYPE(ocudu_dapp_channel_estimator_input_v1);
  PRINT_TYPE(ocudu_dapp_channel_estimator_output_v1);
  PRINT_TYPE(ocudu_dapp_channel_estimator_interface_v1);
  PRINT_TYPE(ocudu_dapp_equalizer_input_v1);
  PRINT_TYPE(ocudu_dapp_equalizer_output_v1);
  PRINT_TYPE(ocudu_dapp_equalizer_interface_v1);
  PRINT_TYPE(ocudu_dapp_receiver_input_v1);
  PRINT_TYPE(ocudu_dapp_receiver_metrics_v1);
  PRINT_TYPE(ocudu_dapp_receiver_output_v1);
  PRINT_TYPE(ocudu_dapp_receiver_interface_v1);
  PRINT_TYPE(ocudu_dapp_scheduler_candidate_v1);
  PRINT_TYPE(ocudu_dapp_scheduler_input_v1);
  PRINT_TYPE(ocudu_dapp_scheduler_intent_v1);
  PRINT_TYPE(ocudu_dapp_scheduler_output_v1);
  PRINT_TYPE(ocudu_dapp_scheduler_interface_v1);
  PRINT_TYPE(ocudu_dapp_spectrum_slot_metadata_v1);
  PRINT_TYPE(ocudu_dapp_spectrum_input_v1);
  PRINT_TYPE(ocudu_dapp_spectrum_interference_map_v1);
  PRINT_TYPE(ocudu_dapp_spectrum_interface_v1);
  PRINT_TYPE(ocudu_dapp_srs_isac_metadata_v1);
  PRINT_TYPE(ocudu_dapp_srs_isac_input_v1);
  PRINT_TYPE(ocudu_dapp_srs_isac_cir_preview_v1);
  PRINT_TYPE(ocudu_dapp_srs_isac_interface_v1);
  PRINT_TYPE(ocudu_dapp_ul_resource_span_v1);
  PRINT_TYPE(ocudu_dapp_ul_resource_plan_v1);
  PRINT_TYPE(ocudu_dapp_quiet_period_intent_v1);
  PRINT_TYPE(ocudu_dapp_quiet_period_reservation_v1);
  PRINT_TYPE(ocudu_dapp_control_header_v1);
  PRINT_TYPE(ocudu_dapp_route_offer_v1);
  PRINT_TYPE(ocudu_dapp_route_ready_v1);
  PRINT_TYPE(ocudu_dapp_process_interface_binding_v1);
  PRINT_TYPE(ocudu_dapp_route_bundle_v1);
  PRINT_TYPE(ocudu_dapp_route_bundle_ready_v1);
  PRINT_TYPE(ocudu_dapp_publication_record_header_v1);
  PRINT_TYPE(ocudu_dapp_cuda_export_header_v1);
  PRINT_TYPE(ocudu_dapp_cuda_export_slot_v1);

  PRINT_FIELD(ocudu_dapp_tensor_view_v1, native_handle);
  PRINT_FIELD(ocudu_dapp_tensor_view_v1, dimensions);
  PRINT_FIELD(ocudu_dapp_tensor_view_v1, layout_id);
  PRINT_FIELD(ocudu_dapp_tensor_view_v1, completion_token);
  PRINT_FIELD(ocudu_dapp_tensor_view_v1, reserved);
  PRINT_FIELD(ocudu_dapp_invocation_v1, execution_stream);
  PRINT_FIELD(ocudu_dapp_invocation_v1, reserved);
  PRINT_FIELD(ocudu_dapp_worker_api_v1, create_worker_context);
  PRINT_FIELD(ocudu_dapp_worker_api_v1, reserved);
  PRINT_FIELD(ocudu_dapp_interface_descriptor_v1, interface_vtable);
  PRINT_FIELD(ocudu_dapp_interface_descriptor_v1, use_case_profile);
  PRINT_FIELD(ocudu_dapp_interface_descriptor_v1, reserved);
  PRINT_FIELD(ocudu_dapp_module_v1, interfaces);
  PRINT_FIELD(ocudu_dapp_module_v1, create);
  PRINT_FIELD(ocudu_dapp_module_v1, management);
  PRINT_FIELD(ocudu_dapp_module_v1, reserved);
  PRINT_FIELD(ocudu_dapp_pusch_metadata_v1, data_re_mask);
  PRINT_FIELD(ocudu_dapp_pusch_metadata_v1, data_re_indices);
  PRINT_FIELD(ocudu_dapp_pusch_metadata_v1, reserved);
  PRINT_FIELD(ocudu_dapp_dmrs_metadata_v1, reference_symbols);
  PRINT_FIELD(ocudu_dapp_dmrs_metadata_v1, coordinates);
  PRINT_FIELD(ocudu_dapp_dmrs_metadata_v1, reserved);
  PRINT_FIELD(ocudu_dapp_channel_estimator_input_v1, pusch);
  PRINT_FIELD(ocudu_dapp_channel_estimator_output_v1, channel_estimates);
  PRINT_FIELD(ocudu_dapp_equalizer_output_v1, equalized_symbols);
  PRINT_FIELD(ocudu_dapp_equalizer_output_v1, post_equalization_noise);
  PRINT_FIELD(ocudu_dapp_receiver_metrics_v1, post_equalization_noise);
  PRINT_FIELD(ocudu_dapp_receiver_metrics_v1, reserved);
  PRINT_FIELD(ocudu_dapp_receiver_output_v1, llrs);
  PRINT_FIELD(ocudu_dapp_receiver_output_v1, metrics);
  PRINT_FIELD(ocudu_dapp_receiver_output_v1, confidence_q16);
  PRINT_FIELD(ocudu_dapp_receiver_output_v1, reserved0);
  PRINT_FIELD(ocudu_dapp_scheduler_candidate_v1, hol_delay_us);
  PRINT_FIELD(ocudu_dapp_scheduler_candidate_v1, average_rate_bps);
  PRINT_FIELD(ocudu_dapp_scheduler_candidate_v1, bler);
  PRINT_FIELD(ocudu_dapp_scheduler_candidate_v1, bler_nof_observations);
  PRINT_FIELD(ocudu_dapp_scheduler_candidate_v1, reserved);
  PRINT_FIELD(ocudu_dapp_scheduler_input_v1, candidates);
  PRINT_FIELD(ocudu_dapp_scheduler_input_v1, optional_interference_mask);
  PRINT_FIELD(ocudu_dapp_scheduler_output_v1, intents);
  PRINT_FIELD(ocudu_dapp_scheduler_output_v1, avoid_prb_mask);
  PRINT_FIELD(ocudu_dapp_spectrum_input_v1, uplink_resource_grid);
  PRINT_FIELD(ocudu_dapp_spectrum_interference_map_v1, prb_confidence);
  PRINT_FIELD(ocudu_dapp_srs_isac_input_v1, channel_estimates);
  PRINT_FIELD(ocudu_dapp_srs_isac_cir_preview_v1, tap_power);
  PRINT_FIELD(ocudu_dapp_route_offer_v1, transport_kind);
  PRINT_FIELD(ocudu_dapp_route_offer_v1, backend_id);
  PRINT_FIELD(ocudu_dapp_route_offer_v1, export_slot_count);
  PRINT_FIELD(ocudu_dapp_route_offer_v1, export_slot_capacity);
  PRINT_FIELD(ocudu_dapp_process_interface_binding_v1, route_count);
  PRINT_FIELD(ocudu_dapp_process_interface_binding_v1, state);
  PRINT_FIELD(ocudu_dapp_route_bundle_v1, interfaces);
  PRINT_FIELD(ocudu_dapp_route_bundle_v1, route_generation);
  PRINT_FIELD(ocudu_dapp_route_bundle_v1, bundle_id);
  PRINT_FIELD(ocudu_dapp_route_bundle_ready_v1, route_generation);
  PRINT_FIELD(ocudu_dapp_route_bundle_ready_v1, bundle_id);
  PRINT_FIELD(ocudu_dapp_publication_record_header_v1, tensor_offset);
  PRINT_FIELD(ocudu_dapp_publication_record_header_v1, publication_sequence);
  PRINT_FIELD(ocudu_dapp_cuda_export_header_v1, slot_count);
  PRINT_FIELD(ocudu_dapp_cuda_export_header_v1, slot_capacity);
  PRINT_FIELD(ocudu_dapp_cuda_export_slot_v1, memory_handle);
  PRINT_FIELD(ocudu_dapp_cuda_export_slot_v1, ready_event_handle);
  return 0;
}
