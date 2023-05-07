// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include "core/frontend/applets/applet.h"
#include "core/hle/service/nfp/nfp_types.h"

namespace Service::NFC {
class NfcDevice;
} // namespace Service::NFC

namespace Core::Frontend {

struct CabinetParameters {
    Service::NFP::TagInfo tag_info;
    Service::NFP::RegisterInfo register_info;
    Service::NFP::CabinetMode mode;
};

using CabinetCallback = std::function<void(bool, const std::string&)>;

class CabinetApplet : public Applet {
public:
    virtual ~CabinetApplet();
    virtual void ShowCabinetApplet(const CabinetCallback& callback,
                                   const CabinetParameters& parameters,
                                   std::shared_ptr<Service::NFC::NfcDevice> nfp_device) const = 0;
};

class DefaultCabinetApplet final : public CabinetApplet {
public:
    void Close() const override;
    void ShowCabinetApplet(const CabinetCallback& callback, const CabinetParameters& parameters,
                           std::shared_ptr<Service::NFC::NfcDevice> nfp_device) const override;
};

} // namespace Core::Frontend
