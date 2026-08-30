// SPDX-FileCopyrightText: Copyright (C) 2026 DeepSig Inc.
// SPDX-License-Identifier: BSD-3-Clause-Clear

#include "ocudu_dapp_sdk/process_cuda_client.h"
#include <cstring>
#include <cuda_runtime_api.h>
#include <new>
#include <vector>

using namespace ocudu::dapp::sdk;

namespace {

static_assert(sizeof(cudaIpcMemHandle_t) == OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1);
static_assert(sizeof(cudaIpcEventHandle_t) == OCUDU_DAPP_CUDA_IPC_HANDLE_BYTES_V1);

class route_impl final : public cuda_process_route
{
public:
  static cuda_process_route_create_result create(const process_route& route)
  {
    auto pool = route.cuda_export_pool();
    if (!pool || pool->header == nullptr || pool->slots == nullptr) {
      return {{}, "route has no admitted CUDA export pool"};
    }
    const auto& header = *pool->header;
    if (cudaSetDevice(header.device_ordinal) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return {{}, "unable to select CUDA export device"};
    }
    cudaStream_t stream = nullptr;
    if (cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return {{}, "unable to create CUDA process route stream"};
    }
    bool stream_owned = false;
    try {
      auto result  = std::unique_ptr<route_impl>(new route_impl(header.device_ordinal, stream, header.slot_capacity));
      stream_owned = true;
      result->memory.resize(header.slot_count);
      result->ready.resize(header.slot_count);
      for (uint32_t index = 0; index != header.slot_count; ++index) {
        cudaIpcMemHandle_t   memory_handle{};
        cudaIpcEventHandle_t event_handle{};
        std::memcpy(&memory_handle, pool->slots[index].memory_handle, sizeof(memory_handle));
        std::memcpy(&event_handle, pool->slots[index].ready_event_handle, sizeof(event_handle));
        cudaError_t status =
            cudaIpcOpenMemHandle(&result->memory[index], memory_handle, cudaIpcMemLazyEnablePeerAccess);
        if (status == cudaSuccess) {
          status = cudaIpcOpenEventHandle(&result->ready[index], event_handle);
        }
        if (status != cudaSuccess) {
          const std::string detail = std::string("unable to import CUDA export slot: ") + cudaGetErrorString(status);
          static_cast<void>(cudaGetLastError());
          return {{}, detail};
        }
      }
      return {std::move(result), {}};
    } catch (const std::bad_alloc&) {
      if (!stream_owned) {
        static_cast<void>(cudaStreamDestroy(stream));
      }
      return {{}, "unable to allocate CUDA process route state"};
    } catch (...) {
      if (!stream_owned) {
        static_cast<void>(cudaStreamDestroy(stream));
      }
      return {{}, "unknown CUDA process route construction failure"};
    }
  }

  ~route_impl() override
  {
    static_cast<void>(cudaSetDevice(device));
    static_cast<void>(cudaStreamSynchronize(stream));
    for (cudaEvent_t event : ready) {
      if (event != nullptr) {
        static_cast<void>(cudaEventDestroy(event));
      }
    }
    for (void* pointer : memory) {
      if (pointer != nullptr) {
        static_cast<void>(cudaIpcCloseMemHandle(pointer));
      }
    }
    static_cast<void>(cudaStreamDestroy(stream));
  }

  std::optional<cuda_publication_view>
  prepare(const ocudu_dapp_publication_record_view_v1& publication) noexcept override
  {
    const auto& header = publication.header;
    if ((header.flags & OCUDU_DAPP_PUBLICATION_RECORD_EXTERNAL_TENSOR_V1) == 0 || publication.tensor != nullptr ||
        header.tensor_offset >= memory.size() || header.tensor_size == 0 || header.tensor_size > capacity ||
        cudaSetDevice(device) != cudaSuccess ||
        cudaStreamWaitEvent(stream, ready[header.tensor_offset], 0) != cudaSuccess) {
      static_cast<void>(cudaGetLastError());
      return std::nullopt;
    }
    return cuda_publication_view{static_cast<const uint8_t*>(memory[header.tensor_offset]),
                                 static_cast<std::size_t>(header.tensor_size), reinterpret_cast<uintptr_t>(stream),
                                 device};
  }

private:
  route_impl(uint32_t device_, cudaStream_t stream_, uint64_t capacity_) noexcept :
    device(device_), stream(stream_), capacity(capacity_)
  {
  }

  uint32_t                 device{};
  cudaStream_t             stream{};
  uint64_t                 capacity{};
  std::vector<void*>       memory;
  std::vector<cudaEvent_t> ready;
};

} // namespace

cuda_process_route_create_result cuda_process_route::create(const process_route& route) noexcept
{
  try {
    return route_impl::create(route);
  } catch (const std::bad_alloc&) {
    return {{}, "unable to allocate CUDA process route"};
  } catch (...) {
    return {{}, "unknown CUDA process route failure"};
  }
}
