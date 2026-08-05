/**
Copyright (c) 2026 Huawei Technologies Co., Ltd.
This program is free software, you can redistribute it and/or modify it under the terms and conditions of
CANN Open Software License Agreement Version 2.0 (the "License").
Please refer to the License for details. You may not use this file except in compliance with the License.
THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY, OR FITNESS FOR A PARTICULAR PURPOSE.
See LICENSE in the root of the software repository for the full text of the License.
*/

#ifndef PTO_COMM_TNOTIFY_HPP
#define PTO_COMM_TNOTIFY_HPP

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <dlfcn.h>
#include <pto/common/pto_tile.hpp>
#include <pto/comm/comm_types.hpp>
#include "pto/cpu/tile_offsets.hpp"
#include "pto/common/pto_instr.hpp"
#include "pto/cpu/parallel.hpp"

namespace pto {
namespace comm {

using NotifyOp = ::pto::comm::NotifyOp;

namespace detail {

// Self-notify sinkhole emulation.
//
// On real NPU hardware TNOTIFY is a remote-write interconnect instruction:
// a TNOTIFY whose destination resolves to the *sender's own* rank window is
// dropped by the interconnect (self-notify is a no-op).  The CPU simulator
// cannot see that distinction from the destination pointer alone, so the sim
// runtime (libcpu_sim_context.so) registers this rank's own window slice and
// exposes `pto_sim_is_self_notify_sinkhole`.  We resolve that hook once and
// drop the write when it fires, so a kernel that (incorrectly) TNOTIFYs its
// own window fails in simulation exactly as it does onboard.
inline bool IsSelfNotifySinkhole(uint64_t addr)
{
    static auto hook =
        reinterpret_cast<bool (*)(uint64_t)>(dlsym(RTLD_DEFAULT, "pto_sim_is_self_notify_sinkhole"));
    return hook != nullptr && hook(addr);
}

} // namespace detail

template <typename GlobalSignalData>
void TNotify_Impl(typename GlobalSignalData::DType* dstSignalData, int32_t value, NotifyOp op)
{
    using DType = typename GlobalSignalData::DType;
    if (detail::IsSelfNotifySinkhole(reinterpret_cast<uint64_t>(dstSignalData))) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            std::fprintf(stderr,
                         "[cpu-sim] TNOTIFY to self window dropped (addr=%p): "
                         "onboard NPU self-notify is a no-op; "
                         "use a local store for self-destination data\n",
                         static_cast<void *>(dstSignalData));
        }
        return;
    }
    auto* atomicPtr = reinterpret_cast<std::atomic<DType>*>(dstSignalData);
    switch (op) {
        case NotifyOp::AtomicAdd:
            atomicPtr->fetch_add(static_cast<DType>(value), std::memory_order_seq_cst);
            break;
        case NotifyOp::Set:
            atomicPtr->store(static_cast<DType>(value), std::memory_order_seq_cst);
            break;
        default:
            break;
    }
}

template <typename GlobalSignalData>
PTO_INTERNAL void TNOTIFY_IMPL(GlobalSignalData& dstSignalData, int32_t value, NotifyOp op)
{
    TNotify_Impl<GlobalSignalData>(dstSignalData.data(), value, op);
}

} // namespace comm
} // namespace pto
#endif
