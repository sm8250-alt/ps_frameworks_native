/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "ServiceManager.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <binder/BpBinder.h>
#include <binder/IPCThreadState.h>
#include <binder/ProcessState.h>
#include <binder/Stability.h>
#include <cutils/android_filesystem_config.h>
#include <cutils/multiuser.h>
#include <thread>

#ifndef VENDORSERVICEMANAGER
#include <vintf/VintfObject.h>
#ifdef __ANDROID_RECOVERY__
#include <vintf/VintfObjectRecovery.h>
#endif // __ANDROID_RECOVERY__
#include <vintf/constants.h>
#endif  // !VENDORSERVICEMANAGER

using ::android::binder::Status;
using ::android::internal::Stability;

namespace android {

#ifndef VENDORSERVICEMANAGER

struct ManifestWithDescription {
    std::shared_ptr<const vintf::HalManifest> manifest;
    const char* description;
};
static std::vector<ManifestWithDescription> GetManifestsWithDescription() {
#ifdef __ANDROID_RECOVERY__
    auto vintfObject = vintf::VintfObjectRecovery::GetInstance();
    if (vintfObject == nullptr) {
        ALOGE("NULL VintfObjectRecovery!");
        return {};
    }
    return {ManifestWithDescription{vintfObject->getRecoveryHalManifest(), "recovery"}};
#else
    auto vintfObject = vintf::VintfObject::GetInstance();
    if (vintfObject == nullptr) {
        ALOGE("NULL VintfObject!");
        return {};
    }
    return {ManifestWithDescription{vintfObject->getDeviceHalManifest(), "device"},
            ManifestWithDescription{vintfObject->getFrameworkHalManifest(), "framework"}};
#endif
}

// func true -> stop search and forEachManifest will return true
static bool forEachManifest(const std::function<bool(const ManifestWithDescription&)>& func) {
    for (const ManifestWithDescription& mwd : GetManifestsWithDescription()) {
        if (mwd.manifest == nullptr) {
            ALOGE("NULL VINTF MANIFEST!: %s", mwd.description);
            // note, we explicitly do not retry here, so that we can detect VINTF
            // or other bugs (b/151696835)
            continue;
        }
        if (func(mwd)) return true;
    }
    return false;
}

struct AidlName {
    std::string package;
    std::string iface;
    std::string instance;

    static bool fill(const std::string& name, AidlName* aname) {
        size_t firstSlash = name.find('/');
        size_t lastDot = name.rfind('.', firstSlash);
        if (firstSlash == std::string::npos || lastDot == std::string::npos) {
            ALOGE("VINTF HALs require names in the format type/instance (e.g. "
                  "some.package.foo.IFoo/default) but got: %s",
                  name.c_str());
            return false;
        }
        aname->package = name.substr(0, lastDot);
        aname->iface = name.substr(lastDot + 1, firstSlash - lastDot - 1);
        aname->instance = name.substr(firstSlash + 1);
        return true;
    }
};

static bool isVintfDeclared(const std::string& name) {
    AidlName aname;
    if (!AidlName::fill(name, &aname)) return false;

    bool found = forEachManifest([&](const ManifestWithDescription& mwd) {
        if (mwd.manifest->hasAidlInstance(aname.package, aname.iface, aname.instance)) {
            ALOGI("Found %s in %s VINTF manifest.", name.c_str(), mwd.description);
            return true; // break
        }
        return false;  // continue
    });

    if (!found) {
        // Although it is tested, explicitly rebuilding qualified name, in case it
        // becomes something unexpected.
        ALOGI("Could not find %s.%s/%s in the VINTF manifest.", aname.package.c_str(),
              aname.iface.c_str(), aname.instance.c_str());
    }

    return found;
}

static std::optional<std::string> getVintfUpdatableApex(const std::string& name) {
    AidlName aname;
    if (!AidlName::fill(name, &aname)) return std::nullopt;

    std::optional<std::string> updatableViaApex;

    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() != vintf::HalFormat::AIDL) return true;
            if (manifestInstance.package() != aname.package) return true;
            if (manifestInstance.interface() != aname.iface) return true;
            if (manifestInstance.instance() != aname.instance) return true;
            updatableViaApex = manifestInstance.updatableViaApex();
            return false; // break (libvintf uses opposite convention)
        });
        if (updatableViaApex.has_value()) return true; // break (found match)
        return false; // continue
    });

    return updatableViaApex;
}

static std::vector<std::string> getVintfUpdatableInstances(const std::string& apexName) {
    std::vector<std::string> instances;

    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() == vintf::HalFormat::AIDL &&
                manifestInstance.updatableViaApex().has_value() &&
                manifestInstance.updatableViaApex().value() == apexName) {
                std::string aname = manifestInstance.package() + "." +
                        manifestInstance.interface() + "/" + manifestInstance.instance();
                instances.push_back(aname);
            }
            return true; // continue (libvintf uses opposite convention)
        });
        return false; // continue
    });

    return instances;
}

static std::optional<ConnectionInfo> getVintfConnectionInfo(const std::string& name) {
    AidlName aname;
    if (!AidlName::fill(name, &aname)) return std::nullopt;

    std::optional<std::string> ip;
    std::optional<uint64_t> port;
    forEachManifest([&](const ManifestWithDescription& mwd) {
        mwd.manifest->forEachInstance([&](const auto& manifestInstance) {
            if (manifestInstance.format() != vintf::HalFormat::AIDL) return true;
            if (manifestInstance.package() != aname.package) return true;
            if (manifestInstance.interface() != aname.iface) return true;
            if (manifestInstance.instance() != aname.instance) return true;
            ip = manifestInstance.ip();
            port = manifestInstance.port();
            return false; // break (libvintf uses opposite convention)
        });
        return false; // continue
    });

    if (ip.has_value() && port.has_value()) {
        ConnectionInfo info;
        info.ipAddress = *ip;
        info.port = *port;
        return std::make_optional<ConnectionInfo>(info);
    } else {
        return std::nullopt;
    }
}

static std::vector<std::string> getVintfInstances(const std::string& interface) {
    size_t lastDot = interface.rfind('.');
    if (lastDot == std::string::npos) {
        ALOGE("VINTF interfaces require names in Java package format (e.g. some.package.foo.IFoo) "
              "but got: %s",
              interface.c_str());
        return {};
    }
    const std::string package = interface.substr(0, lastDot);
    const std::string iface = interface.substr(lastDot+1);

    std::vector<std::string> ret;
    (void)forEachManifest([&](const ManifestWithDescription& mwd) {
        auto instances = mwd.manifest->getAidlInstances(package, iface);
        ret.insert(ret.end(), instances.begin(), instances.end());
        return false;  // continue
    });

    return ret;
}

static bool meetsDeclarationRequirements(const sp<IBinder>& binder, const std::string& name) {
    if (!Stability::requiresVintfDeclaration(binder)) {
        return true;
    }

    return isVintfDeclared(name);
}
#endif  // !VENDORSERVICEMANAGER

ServiceManager::ServiceManager(std::unique_ptr<Access>&& access) : mAccess(std::move(access)) {
// TODO(b/151696835): reenable performance hack when we solve bug, since with
//     this hack and other fixes, it is unlikely we will see even an ephemeral
//     failure when the manifest parse fails. The goal is that the manifest will
//     be read incorrectly and cause the process trying to register a HAL to
//     fail. If this is in fact an early boot kernel contention issue, then we
//     will get no failure, and by its absence, be signalled to invest more
//     effort in re-adding this performance hack.
// #ifndef VENDORSERVICEMANAGER
//     // can process these at any times, don't want to delay first VINTF client
//     std::thread([] {
//         vintf::VintfObject::GetDeviceHalManifest();
//         vintf::VintfObject::GetFrameworkHalManifest();
//     }).detach();
// #endif  // !VENDORSERVICEMANAGER
}
ServiceManager::~ServiceManager() {
    // this should only happen in tests

    for (const auto& [name, callbacks] : mNameToRegistrationCallback) {
        CHECK(!callbacks.empty()) << name;
        for (const auto& callback : callbacks) {
            CHECK(callback != nullptr) << name;
        }
    }

    for (const auto& [name, service] : mNameToService) {
        CHECK(service.binder != nullptr) << name;
    }
}

Status ServiceManager::getService(const std::string& name, sp<IBinder>* outBinder) {
    *outBinder = tryGetService(name, true);
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

Status ServiceManager::checkService(const std::string& name, sp<IBinder>* outBinder) {
    *outBinder = tryGetService(name, false);
    // returns ok regardless of result for legacy reasons
    return Status::ok();
}

sp<IBinder> ServiceManager::tryGetService(const std::string& name, bool startIfNotFound) {
    auto ctx = mAccess->getCallingContext();

    sp<IBinder> out;
    Service* service = nullptr;
    if (auto it = mNameToService.find(name); it != mNameToService.end()) {
        service = &(it->second);

        if (!service->allowIsolated) {
            uid_t appid = multiuser_get_app_id(ctx.uid);
            bool isIsolated = appid >= AID_ISOLATED_START && appid <= AID_ISOLATED_END;

            if (isIsolated) {
                return nullptr;
            }
        }
        out = service->binder;
    }

    if (!mAccess->canFind(ctx, name)) {
        return nullptr;
    }

    if (!out && startIfNotFound) {
        tryStartService(name);
    }

    if (out) {
        // Setting this guarantee each time we hand out a binder ensures that the client-checking
        // loop knows about the event even if the client immediately drops the service
        service->guaranteeClient = true;
    }

    return out;
}

bool isValidServiceName(const std::string& name) {
    if (name.size() == 0) return false;
    if (name.size() > 127) return false;

    for (char c : name) {
        if (c == '_' || c == '-' || c == '.' || c == '/') continue;
        if (c >= 'a' && c <= 'z') continue;
        if (c >= 'A' && c <= 'Z') continue;
        if (c >= '0' && c <= '9') continue;
        return false;
    }

    return true;
}

Status ServiceManager::addService(const std::string& name, const sp<IBinder>& binder, bool allowIsolated, int32_t dumpPriority) {
    auto ctx = mAccess->getCallingContext();

    if (multiuser_get_app_id(ctx.uid) >= AID_APP) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "App UIDs cannot add services");
    }

    if (!mAccess->canAdd(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denial");
    }

    if (binder == nullptr) {
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Null binder");
    }

    if (!isValidServiceName(name)) {
        ALOGE("Invalid service name: %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "Invalid service name");
    }

#ifndef VENDORSERVICEMANAGER
    if (!meetsDeclarationRequirements(binder, name)) {
        // already logged
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT, "VINTF declaration error");
    }
#endif  // !VENDORSERVICEMANAGER

    // implicitly unlinked when the binder is removed
    if (binder->remoteBinder() != nullptr &&
        binder->linkToDeath(sp<ServiceManager>::fromExisting(this)) != OK) {
        ALOGE("Could not linkToDeath when adding %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE, "linkToDeath failure");
    }

    auto it = mNameToService.find(name);
    if (it != mNameToService.end()) {
        const Service& existing = it->second;

        // We could do better than this because if the other service dies, it
        // may not have an entry here. However, this case is unlikely. We are
        // only trying to detect when two different services are accidentally installed.

        if (existing.ctx.uid != ctx.uid) {
            ALOGW("Service '%s' originally registered from UID %u but it is now being registered "
                  "from UID %u. Multiple instances installed?",
                  name.c_str(), existing.ctx.uid, ctx.uid);
        }

        if (existing.ctx.sid != ctx.sid) {
            ALOGW("Service '%s' originally registered from SID %s but it is now being registered "
                  "from SID %s. Multiple instances installed?",
                  name.c_str(), existing.ctx.sid.c_str(), ctx.sid.c_str());
        }

        ALOGI("Service '%s' originally registered from PID %d but it is being registered again "
              "from PID %d. Bad state? Late death notification? Multiple instances installed?",
              name.c_str(), existing.ctx.debugPid, ctx.debugPid);
    }

    // Overwrite the old service if it exists
    mNameToService[name] = Service{
            .binder = binder,
            .allowIsolated = allowIsolated,
            .dumpPriority = dumpPriority,
            .ctx = ctx,
    };

    if (auto it = mNameToRegistrationCallback.find(name); it != mNameToRegistrationCallback.end()) {
        for (const sp<IServiceCallback>& cb : it->second) {
            mNameToService[name].guaranteeClient = true;
            // permission checked in registerForNotifications
            cb->onRegistration(name, binder);
        }
    }

    return Status::ok();
}

Status ServiceManager::listServices(int32_t dumpPriority, std::vector<std::string>* outList) {
    if (!mAccess->canList(mAccess->getCallingContext())) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    size_t toReserve = 0;
    for (auto const& [name, service] : mNameToService) {
        (void) name;

        if (service.dumpPriority & dumpPriority) ++toReserve;
    }

    CHECK(outList->empty());

    outList->reserve(toReserve);
    for (auto const& [name, service] : mNameToService) {
        (void) service;

        if (service.dumpPriority & dumpPriority) {
            outList->push_back(name);
        }
    }

    return Status::ok();
}

Status ServiceManager::registerForNotifications(
        const std::string& name, const sp<IServiceCallback>& callback) {
    auto ctx = mAccess->getCallingContext();

    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    if (!isValidServiceName(name)) {
        ALOGE("Invalid service name: %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    }

    if (callback == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER);
    }

    if (OK !=
        IInterface::asBinder(callback)->linkToDeath(
                sp<ServiceManager>::fromExisting(this))) {
        ALOGE("Could not linkToDeath when adding %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    mNameToRegistrationCallback[name].push_back(callback);

    if (auto it = mNameToService.find(name); it != mNameToService.end()) {
        const sp<IBinder>& binder = it->second.binder;

        // never null if an entry exists
        CHECK(binder != nullptr) << name;
        callback->onRegistration(name, binder);
    }

    return Status::ok();
}
Status ServiceManager::unregisterForNotifications(
        const std::string& name, const sp<IServiceCallback>& callback) {
    auto ctx = mAccess->getCallingContext();

    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    bool found = false;

    auto it = mNameToRegistrationCallback.find(name);
    if (it != mNameToRegistrationCallback.end()) {
        removeRegistrationCallback(IInterface::asBinder(callback), &it, &found);
    }

    if (!found) {
        ALOGE("Trying to unregister callback, but none exists %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    return Status::ok();
}

Status ServiceManager::isDeclared(const std::string& name, bool* outReturn) {
    auto ctx = mAccess->getCallingContext();

    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    *outReturn = false;

#ifndef VENDORSERVICEMANAGER
    *outReturn = isVintfDeclared(name);
#endif
    return Status::ok();
}

binder::Status ServiceManager::getDeclaredInstances(const std::string& interface, std::vector<std::string>* outReturn) {
    auto ctx = mAccess->getCallingContext();

    std::vector<std::string> allInstances;
#ifndef VENDORSERVICEMANAGER
    allInstances = getVintfInstances(interface);
#endif

    outReturn->clear();

    for (const std::string& instance : allInstances) {
        if (mAccess->canFind(ctx, interface + "/" + instance)) {
            outReturn->push_back(instance);
        }
    }

    if (outReturn->size() == 0 && allInstances.size() != 0) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    return Status::ok();
}

Status ServiceManager::updatableViaApex(const std::string& name,
                                        std::optional<std::string>* outReturn) {
    auto ctx = mAccess->getCallingContext();

    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    *outReturn = std::nullopt;

#ifndef VENDORSERVICEMANAGER
    *outReturn = getVintfUpdatableApex(name);
#endif
    return Status::ok();
}

Status ServiceManager::getUpdatableNames([[maybe_unused]] const std::string& apexName,
                                         std::vector<std::string>* outReturn) {
    auto ctx = mAccess->getCallingContext();

    std::vector<std::string> apexUpdatableInstances;
#ifndef VENDORSERVICEMANAGER
    apexUpdatableInstances = getVintfUpdatableInstances(apexName);
#endif

    outReturn->clear();

    for (const std::string& instance : apexUpdatableInstances) {
        if (mAccess->canFind(ctx, instance)) {
            outReturn->push_back(instance);
        }
    }

    if (outReturn->size() == 0 && apexUpdatableInstances.size() != 0) {
        return Status::fromExceptionCode(Status::EX_SECURITY, "SELinux denial");
    }

    return Status::ok();
}

Status ServiceManager::getConnectionInfo(const std::string& name,
                                         std::optional<ConnectionInfo>* outReturn) {
    auto ctx = mAccess->getCallingContext();

    if (!mAccess->canFind(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    *outReturn = std::nullopt;

#ifndef VENDORSERVICEMANAGER
    *outReturn = getVintfConnectionInfo(name);
#endif
    return Status::ok();
}

void ServiceManager::removeRegistrationCallback(const wp<IBinder>& who,
                                    ServiceCallbackMap::iterator* it,
                                    bool* found) {
    std::vector<sp<IServiceCallback>>& listeners = (*it)->second;

    for (auto lit = listeners.begin(); lit != listeners.end();) {
        if (IInterface::asBinder(*lit) == who) {
            if(found) *found = true;
            lit = listeners.erase(lit);
        } else {
            ++lit;
        }
    }

    if (listeners.empty()) {
        *it = mNameToRegistrationCallback.erase(*it);
    } else {
        (*it)++;
    }
}

void ServiceManager::binderDied(const wp<IBinder>& who) {
    for (auto it = mNameToService.begin(); it != mNameToService.end();) {
        if (who == it->second.binder) {
            it = mNameToService.erase(it);
        } else {
            ++it;
        }
    }

    for (auto it = mNameToRegistrationCallback.begin(); it != mNameToRegistrationCallback.end();) {
        removeRegistrationCallback(who, &it, nullptr /*found*/);
    }

    for (auto it = mNameToClientCallback.begin(); it != mNameToClientCallback.end();) {
        removeClientCallback(who, &it);
    }
}

void ServiceManager::tryStartService(const std::string& name) {
    ALOGI("Since '%s' could not be found, trying to start it as a lazy AIDL service. (if it's not "
          "configured to be a lazy service, it may be stuck starting or still starting).",
          name.c_str());

    std::thread([=] {
        if (!base::SetProperty("ctl.interface_start", "aidl/" + name)) {
            ALOGI("Tried to start aidl service %s as a lazy service, but was unable to. Usually "
                  "this happens when a "
                  "service is not installed, but if the service is intended to be used as a "
                  "lazy service, then it may be configured incorrectly.",
                  name.c_str());
        }
    }).detach();
}

Status ServiceManager::registerClientCallback(const std::string& name, const sp<IBinder>& service,
                                              const sp<IClientCallback>& cb) {
    if (cb == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER);
    }

    auto ctx = mAccess->getCallingContext();
    if (!mAccess->canAdd(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    auto serviceIt = mNameToService.find(name);
    if (serviceIt == mNameToService.end()) {
        ALOGE("Could not add callback for nonexistent service: %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    }

    if (serviceIt->second.ctx.debugPid != IPCThreadState::self()->getCallingPid()) {
        ALOGW("Only a server can register for client callbacks (for %s)", name.c_str());
        return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION);
    }

    if (serviceIt->second.binder != service) {
        ALOGW("Tried to register client callback for %s but a different service is registered "
              "under this name.",
              name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_ARGUMENT);
    }

    if (OK !=
        IInterface::asBinder(cb)->linkToDeath(sp<ServiceManager>::fromExisting(this))) {
        ALOGE("Could not linkToDeath when adding client callback for %s", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    mNameToClientCallback[name].push_back(cb);

    return Status::ok();
}

void ServiceManager::removeClientCallback(const wp<IBinder>& who,
                                          ClientCallbackMap::iterator* it) {
    std::vector<sp<IClientCallback>>& listeners = (*it)->second;

    for (auto lit = listeners.begin(); lit != listeners.end();) {
        if (IInterface::asBinder(*lit) == who) {
            lit = listeners.erase(lit);
        } else {
            ++lit;
        }
    }

    if (listeners.empty()) {
        *it = mNameToClientCallback.erase(*it);
    } else {
        (*it)++;
    }
}

ssize_t ServiceManager::Service::getNodeStrongRefCount() {
    sp<BpBinder> bpBinder = sp<BpBinder>::fromExisting(binder->remoteBinder());
    if (bpBinder == nullptr) return -1;

    return ProcessState::self()->getStrongRefCountForNode(bpBinder);
}

void ServiceManager::handleClientCallbacks() {
    for (const auto& [name, service] : mNameToService) {
        handleServiceClientCallback(name, true);
    }
}

ssize_t ServiceManager::handleServiceClientCallback(const std::string& serviceName,
                                                    bool isCalledOnInterval) {
    auto serviceIt = mNameToService.find(serviceName);
    if (serviceIt == mNameToService.end() || mNameToClientCallback.count(serviceName) < 1) {
        return -1;
    }

    Service& service = serviceIt->second;
    ssize_t count = service.getNodeStrongRefCount();

    // binder driver doesn't support this feature
    if (count == -1) return count;

    bool hasClients = count > 1; // this process holds a strong count

    if (service.guaranteeClient) {
        // we have no record of this client
        if (!service.hasClients && !hasClients) {
            sendClientCallbackNotifications(serviceName, true,
                                            "service is guaranteed to be in use");
        }

        // guarantee is temporary
        service.guaranteeClient = false;
    }

    // only send notifications if this was called via the interval checking workflow
    if (isCalledOnInterval) {
        if (hasClients && !service.hasClients) {
            // client was retrieved in some other way
            sendClientCallbackNotifications(serviceName, true, "we now have a record of a client");
        }

        // there are no more clients, but the callback has not been called yet
        if (!hasClients && service.hasClients) {
            sendClientCallbackNotifications(serviceName, false,
                                            "we now have no record of a client");
        }
    }

    return count;
}

void ServiceManager::sendClientCallbackNotifications(const std::string& serviceName,
                                                     bool hasClients, const char* context) {
    auto serviceIt = mNameToService.find(serviceName);
    if (serviceIt == mNameToService.end()) {
        ALOGW("sendClientCallbackNotifications could not find service %s when %s",
              serviceName.c_str(), context);
        return;
    }
    Service& service = serviceIt->second;

    CHECK(hasClients != service.hasClients)
            << "Record shows: " << service.hasClients
            << " so we can't tell clients again that we have client: " << hasClients
            << " when: " << context;

    ALOGI("Notifying %s they %s have clients when %s", serviceName.c_str(),
          hasClients ? "do" : "don't", context);

    auto ccIt = mNameToClientCallback.find(serviceName);
    CHECK(ccIt != mNameToClientCallback.end())
            << "sendClientCallbackNotifications could not find callbacks for service when "
            << context;

    for (const auto& callback : ccIt->second) {
        callback->onClients(service.binder, hasClients);
    }

    service.hasClients = hasClients;
}

Status ServiceManager::tryUnregisterService(const std::string& name, const sp<IBinder>& binder) {
    if (binder == nullptr) {
        return Status::fromExceptionCode(Status::EX_NULL_POINTER);
    }

    auto ctx = mAccess->getCallingContext();
    if (!mAccess->canAdd(ctx, name)) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    auto serviceIt = mNameToService.find(name);
    if (serviceIt == mNameToService.end()) {
        ALOGW("Tried to unregister %s, but that service wasn't registered to begin with.",
              name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    if (serviceIt->second.ctx.debugPid != IPCThreadState::self()->getCallingPid()) {
        ALOGW("Only a server can unregister itself (for %s)", name.c_str());
        return Status::fromExceptionCode(Status::EX_UNSUPPORTED_OPERATION);
    }

    sp<IBinder> storedBinder = serviceIt->second.binder;

    if (binder != storedBinder) {
        ALOGW("Tried to unregister %s, but a different service is registered under this name.",
              name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    if (serviceIt->second.guaranteeClient) {
        ALOGI("Tried to unregister %s, but there is about to be a client.", name.c_str());
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    int clients = handleServiceClientCallback(name, false);

    // clients < 0: feature not implemented or other error. Assume clients.
    // Otherwise:
    // - kernel driver will hold onto one refcount (during this transaction)
    // - servicemanager has a refcount (guaranteed by this transaction)
    // So, if clients > 2, then at least one other service on the system must hold a refcount.
    if (clients < 0 || clients > 2) {
        // client callbacks are either disabled or there are other clients
        ALOGI("Tried to unregister %s, but there are clients: %d", name.c_str(), clients);
        // Set this flag to ensure the clients are acknowledged in the next callback
        serviceIt->second.guaranteeClient = true;
        return Status::fromExceptionCode(Status::EX_ILLEGAL_STATE);
    }

    mNameToService.erase(name);

    return Status::ok();
}

Status ServiceManager::getServiceDebugInfo(std::vector<ServiceDebugInfo>* outReturn) {
    if (!mAccess->canList(mAccess->getCallingContext())) {
        return Status::fromExceptionCode(Status::EX_SECURITY);
    }

    outReturn->reserve(mNameToService.size());
    for (auto const& [name, service] : mNameToService) {
        ServiceDebugInfo info;
        info.name = name;
        info.debugPid = service.ctx.debugPid;

        outReturn->push_back(std::move(info));
    }

    return Status::ok();
}

void ServiceManager::clear() {
    mNameToService.clear();
    mNameToRegistrationCallback.clear();
    mNameToClientCallback.clear();
}

}  // namespace android
