// Copyright 2020 yuzu Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// This file references various implementation details from Atmosphere, an open-source firmware for
// the Nintendo Switch. Copyright 2018-2020 Atmosphere-NX.

#pragma once

#include <array>
#include "common/common_types.h"
#include "core/hle/kernel/k_light_condition_variable.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/object.h"

union ResultCode;

namespace Core {
class System;
}

namespace Kernel {
class KernelCore;
enum class LimitableResource : u32 {
    PhysicalMemoryMax = 0,
    ThreadCountMax = 1,
    EventCountMax = 2,
    TransferMemoryCountMax = 3,
    SessionCountMax = 4,

    Count,
};

constexpr bool IsValidResourceType(LimitableResource type) {
    return type < LimitableResource::Count;
}

class KResourceLimit final : public Object {
public:
    KResourceLimit(KernelCore& kernel, Core::System& system);
    ~KResourceLimit();

    s64 GetLimitValue(LimitableResource which) const;
    s64 GetCurrentValue(LimitableResource which) const;
    s64 GetPeakValue(LimitableResource which) const;
    s64 GetFreeValue(LimitableResource which) const;

    ResultCode SetLimitValue(LimitableResource which, s64 value);

    bool Reserve(LimitableResource which, s64 value);
    bool Reserve(LimitableResource which, s64 value, s64 timeout);
    void Release(LimitableResource which, s64 value);
    void Release(LimitableResource which, s64 value, s64 hint);

    std::string GetTypeName() const override {
        return "KResourceLimit";
    }
    std::string GetName() const override {
        return GetTypeName();
    }

    static constexpr HandleType HANDLE_TYPE = HandleType::ResourceLimit;
    HandleType GetHandleType() const override {
        return HANDLE_TYPE;
    }

    virtual void Finalize() override {}

private:
    std::array<s64, static_cast<std::size_t>(LimitableResource::Count)> limit_values{};
    std::array<s64, static_cast<std::size_t>(LimitableResource::Count)> current_values{};
    std::array<s64, static_cast<std::size_t>(LimitableResource::Count)> current_hints{};
    std::array<s64, static_cast<std::size_t>(LimitableResource::Count)> peak_values{};
    mutable KLightLock lock;
    s32 waiter_count{};
    KLightConditionVariable cond_var;
    KernelCore& kernel;
    Core::System& system;
};
} // namespace Kernel
