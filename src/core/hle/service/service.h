// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <mutex>
#include <string>
#include <boost/container/flat_map.hpp>
#include "common/common_types.h"
#include "core/hle/service/hle_ipc.h"

////////////////////////////////////////////////////////////////////////////////////////////////////
// Namespace Service

namespace Core {
class System;
}

namespace Kernel {
class KServerSession;
class ServiceThread;
} // namespace Kernel

namespace Service {

namespace FileSystem {
class FileSystemController;
}

namespace Nvnflinger {
class HosBinderDriverServer;
class Nvnflinger;
} // namespace Nvnflinger

namespace SM {
class ServiceManager;
}

/// Default number of maximum connections to a server session.
static constexpr u32 ServerSessionCountMax = 0x40;
static_assert(ServerSessionCountMax == 0x40,
              "ServerSessionCountMax isn't 0x40 somehow, this assert is a reminder that this will "
              "break lots of things");

/**
 * This is an non-templated base of ServiceFramework to reduce code bloat and compilation times, it
 * is not meant to be used directly.
 *
 * @see ServiceFramework
 */
class ServiceFrameworkBase : public SessionRequestHandler {
public:
    /// Returns the string identifier used to connect to the service.
    std::string GetServiceName() const {
        return service_name;
    }

    /**
     * Returns the maximum number of sessions that can be connected to this service at the same
     * time.
     */
    u32 GetMaxSessions() const {
        return max_sessions;
    }

    /// Invokes a service request routine using the HIPC protocol.
    void InvokeRequest(HLERequestContext& ctx);

    /// Invokes a service request routine using the HIPC protocol.
    void InvokeRequestTipc(HLERequestContext& ctx);

    /// Handles a synchronization request for the service.
    Result HandleSyncRequest(Kernel::KServerSession& session, HLERequestContext& context) override;

protected:
    /// Member-function pointer type of SyncRequest handlers.
    template <typename Self>
    using HandlerFnP = void (Self::*)(HLERequestContext&);

    /// Used to gain exclusive access to the service members, e.g. from CoreTiming thread.
    [[nodiscard]] std::scoped_lock<std::mutex> LockService() {
        return std::scoped_lock{lock_service};
    }

    /// System context that the service operates under.
    Core::System& system;

    /// Identifier string used to connect to the service.
    std::string service_name;

private:
    template <typename T>
    friend class ServiceFramework;

    struct FunctionInfoBase {
        u32 expected_header;
        HandlerFnP<ServiceFrameworkBase> handler_callback;
        const char* name;
    };

    using InvokerFn = void(ServiceFrameworkBase* object, HandlerFnP<ServiceFrameworkBase> member,
                           HLERequestContext& ctx);

    explicit ServiceFrameworkBase(Core::System& system_, const char* service_name_,
                                  u32 max_sessions_, InvokerFn* handler_invoker_);
    ~ServiceFrameworkBase() override;

    void RegisterHandlersBase(const FunctionInfoBase* functions, std::size_t n);
    void RegisterHandlersBaseTipc(const FunctionInfoBase* functions, std::size_t n);
    void ReportUnimplementedFunction(HLERequestContext& ctx, const FunctionInfoBase* info);

    /// Maximum number of concurrent sessions that this service can handle.
    u32 max_sessions;

    /// Flag to store if a port was already create/installed to detect multiple install attempts,
    /// which is not supported.
    bool service_registered = false;

    /// Function used to safely up-cast pointers to the derived class before invoking a handler.
    InvokerFn* handler_invoker;
    boost::container::flat_map<u32, FunctionInfoBase> handlers;
    boost::container::flat_map<u32, FunctionInfoBase> handlers_tipc;

    /// Used to gain exclusive access to the service members, e.g. from CoreTiming thread.
    std::mutex lock_service;
};

/**
 * Framework for implementing HLE services. Dispatches on the header id of incoming SyncRequests
 * based on a table mapping header ids to handler functions. Service implementations should inherit
 * from ServiceFramework using the CRTP (`class Foo : public ServiceFramework<Foo> { ... };`) and
 * populate it with handlers by calling #RegisterHandlers.
 *
 * In order to avoid duplicating code in the binary and exposing too many implementation details in
 * the header, this class is split into a non-templated base (ServiceFrameworkBase) and a template
 * deriving from it (ServiceFramework). The functions in this class will mostly only erase the type
 * of the passed in function pointers and then delegate the actual work to the implementation in the
 * base class.
 */
template <typename Self>
class ServiceFramework : public ServiceFrameworkBase {
protected:
    /// Contains information about a request type which is handled by the service.
    template <typename T>
    struct FunctionInfoTyped : FunctionInfoBase {
        // TODO(yuriks): This function could be constexpr, but clang is the only compiler that
        // doesn't emit an ICE or a wrong diagnostic because of the static_cast.

        /**
         * Constructs a FunctionInfo for a function.
         *
         * @param expected_header_ request header in the command buffer which will trigger dispatch
         *     to this handler
         * @param handler_callback_ member function in this service which will be called to handle
         *     the request
         * @param name_ human-friendly name for the request. Used mostly for logging purposes.
         */
        FunctionInfoTyped(u32 expected_header_, HandlerFnP<T> handler_callback_, const char* name_)
            : FunctionInfoBase{
                  expected_header_,
                  // Type-erase member function pointer by casting it down to the base class.
                  static_cast<HandlerFnP<ServiceFrameworkBase>>(handler_callback_), name_} {}
    };
    using FunctionInfo = FunctionInfoTyped<Self>;

    /**
     * Initializes the handler with no functions installed.
     *
     * @param system_ The system context to construct this service under.
     * @param service_name_ Name of the service.
     * @param max_sessions_ Maximum number of sessions that can be connected to this service at the
     * same time.
     */
    explicit ServiceFramework(Core::System& system_, const char* service_name_,
                              u32 max_sessions_ = ServerSessionCountMax)
        : ServiceFrameworkBase(system_, service_name_, max_sessions_, Invoker) {}

    /// Registers handlers in the service.
    template <typename T = Self, std::size_t N>
    void RegisterHandlers(const FunctionInfoTyped<T> (&functions)[N]) {
        RegisterHandlers(functions, N);
    }

    /**
     * Registers handlers in the service. Usually prefer using the other RegisterHandlers
     * overload in order to avoid needing to specify the array size.
     */
    template <typename T = Self>
    void RegisterHandlers(const FunctionInfoTyped<T>* functions, std::size_t n) {
        RegisterHandlersBase(functions, n);
    }

    /// Registers handlers in the service.
    template <typename T = Self, std::size_t N>
    void RegisterHandlersTipc(const FunctionInfoTyped<T> (&functions)[N]) {
        RegisterHandlersTipc(functions, N);
    }

    /**
     * Registers handlers in the service. Usually prefer using the other RegisterHandlers
     * overload in order to avoid needing to specify the array size.
     */
    template <typename T = Self>
    void RegisterHandlersTipc(const FunctionInfoTyped<T>* functions, std::size_t n) {
        RegisterHandlersBaseTipc(functions, n);
    }

private:
    /**
     * This function is used to allow invocation of pointers to handlers stored in the base class
     * without needing to expose the type of this derived class. Pointers-to-member may require a
     * fixup when being up or downcast, and thus code that does that needs to know the concrete type
     * of the derived class in order to invoke one of it's functions through a pointer.
     */
    static void Invoker(ServiceFrameworkBase* object, HandlerFnP<ServiceFrameworkBase> member,
                        HLERequestContext& ctx) {
        // Cast back up to our original types and call the member function
        (static_cast<Self*>(object)->*static_cast<HandlerFnP<Self>>(member))(ctx);
    }
};

/**
 * The purpose of this class is to own any objects that need to be shared across the other service
 * implementations. Will be torn down when the global system instance is shutdown.
 */
class Services final {
public:
    explicit Services(std::shared_ptr<SM::ServiceManager>& sm, Core::System& system);
    ~Services();

    void KillNVNFlinger();

private:
    std::unique_ptr<Nvnflinger::HosBinderDriverServer> hos_binder_driver_server;
    std::unique_ptr<Nvnflinger::Nvnflinger> nv_flinger;
};

} // namespace Service
