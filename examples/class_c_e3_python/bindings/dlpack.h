// SPDX-FileCopyrightText: Copyright (c) 2017 by DLPack Contributors
// SPDX-License-Identifier: Apache-2.0
//
// Derived from the DLPack project (https://github.com/dmlc/dlpack),
// distributed under the Apache License 2.0 (see LICENSES/Apache-2.0.txt).
// Trimmed to the minimal C ABI needed to export tensors to PyTorch via
// torch.utils.dlpack.from_dlpack without linking libtorch.

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  kDLCPU         = 1,
  kDLCUDA        = 2,
  kDLCUDAHost    = 3,
  kDLOpenCL      = 4,
  kDLVulkan      = 7,
  kDLMetal       = 8,
  kDLVPI         = 9,
  kDLROCM        = 10,
  kDLExtDev      = 12,
  kDLCUDAManaged = 13,
  kDLOneAPI      = 14,
  kDLWebGPU      = 15,
  kDLHexagon     = 16
} DLDeviceType;

typedef struct {
  DLDeviceType device_type;
  int32_t      device_id;
} DLDevice;

typedef enum {
  kDLInt          = 0U,
  kDLUInt         = 1U,
  kDLFloat        = 2U,
  kDLOpaqueHandle = 3U,
  kDLBfloat       = 4U,
  kDLComplex      = 5U,
  kDLBool         = 6U
} DLDataTypeCode;

typedef struct {
  uint8_t  code;
  uint8_t  bits;
  uint16_t lanes;
} DLDataType;

typedef struct {
  void*      data;
  DLDevice   device;
  int32_t    ndim;
  DLDataType dtype;
  int64_t*   shape;
  int64_t*   strides;
  uint64_t   byte_offset;
} DLTensor;

typedef struct DLManagedTensor {
  DLTensor dl_tensor;
  void*    manager_ctx;
  void (*deleter)(struct DLManagedTensor* self);
} DLManagedTensor;

#ifdef __cplusplus
} // extern "C"
#endif
