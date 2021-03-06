// Copyright 2018 yuzu emulator team
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include "core/hle/service/service.h"

namespace Core {
class System;
}

namespace Service::Audio {

class AudRecU final : public ServiceFramework<AudRecU> {
public:
    explicit AudRecU(Core::System& system_);
    ~AudRecU() override;
};

} // namespace Service::Audio
