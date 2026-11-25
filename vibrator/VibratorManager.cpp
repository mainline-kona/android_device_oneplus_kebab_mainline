/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "KebabRichTapVibratorManager"

#include "VibratorManager.h"
#include "VibrationSession.h"

#include <aidl/android/hardware/vibrator/BnVibratorCallback.h>
#include <android-base/logging.h>
#include <unistd.h>

#include <thread>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr int32_t kDefaultVibratorId = 1;

class VibratorCallback : public BnVibratorCallback {
  public:
    VibratorCallback(int32_t delayMs, std::shared_ptr<IVibrationSession> session,
                     std::shared_ptr<VibratorManager> manager)
        : mDelayMs(delayMs), mSession(std::move(session)), mManager(std::move(manager)) {}
    ndk::ScopedAStatus onComplete() override {
        auto manager = mManager;
        auto session = mSession;
        int32_t delayMs = mDelayMs;
        std::thread([delayMs, session = std::move(session), manager = std::move(manager)]() {
            if (delayMs > 0) {
                usleep(static_cast<useconds_t>(delayMs) * 1000);
            }
            if (manager) {
                manager->clearSession(session);
            }
        }).detach();
        return ndk::ScopedAStatus::ok();
    }

  private:
    const int32_t mDelayMs;
    std::shared_ptr<IVibrationSession> mSession;
    std::shared_ptr<VibratorManager> mManager;
};

ndk::ScopedAStatus VibratorManager::getCapabilities(int32_t* _aidl_return) {
    std::lock_guard lock(mMutex);
    if (mCapabilities == 0) {
        int32_t version = 0;
        if (!getInterfaceVersion(&version).isOk()) {
            return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_ILLEGAL_STATE));
        }
        // Single-vibrator device: only advertise sessions (do not advertise CAP_SYNC
        // without deferred prepare/trigger support).
        if (version >= 3) {
            mCapabilities = IVibratorManager::CAP_START_SESSIONS;
        }
    }
    *_aidl_return = mCapabilities;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibratorIds(std::vector<int32_t>* _aidl_return) {
    *_aidl_return = {kDefaultVibratorId};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::getVibrator(int32_t vibratorId,
                                                std::shared_ptr<IVibrator>* _aidl_return) {
    if (vibratorId == kDefaultVibratorId) {
        *_aidl_return = mDefaultVibrator;
        return ndk::ScopedAStatus::ok();
    }
    *_aidl_return = nullptr;
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
}

ndk::ScopedAStatus VibratorManager::prepareSynced(const std::vector<int32_t>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VibratorManager::triggerSynced(const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VibratorManager::cancelSynced() {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus VibratorManager::startSession(const std::vector<int32_t>& vibratorIds,
                                                 const VibrationSessionConfig&,
                                                 const std::shared_ptr<IVibratorCallback>& callback,
                                                 std::shared_ptr<IVibrationSession>* _aidl_return) {
    *_aidl_return = nullptr;
    int32_t capabilities = 0;
    if (!getCapabilities(&capabilities).isOk()) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    if ((capabilities & IVibratorManager::CAP_START_SESSIONS) == 0) {
        return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
    }
    if (vibratorIds.size() != 1 || vibratorIds[0] != kDefaultVibratorId) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::lock_guard lock(mMutex);
    if (mSession) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_STATE);
    }
    mSessionCallback = callback;
    mSession = ndk::SharedRefBase::make<VibrationSession>(this->ref<VibratorManager>());
    *_aidl_return = mSession;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::clearSessions() {
    abortSession();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibratorManager::startHapticGeneratorSession(
        const std::vector<int32_t>&, const HapticGeneratorConfig&,
        const std::shared_ptr<IVibratorCallback>&, HapticGeneratorSession*) {
    return ndk::ScopedAStatus(AStatus_fromExceptionCode(EX_UNSUPPORTED_OPERATION));
}

void VibratorManager::abortSession() {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        mDefaultVibrator->off();
        clearSession(session);
    }
}

void VibratorManager::closeSession(int32_t delayMs) {
    std::shared_ptr<IVibrationSession> session;
    {
        std::lock_guard lock(mMutex);
        session = mSession;
    }
    if (session) {
        auto self = this->ref<VibratorManager>();
        auto callback = ndk::SharedRefBase::make<VibratorCallback>(delayMs, session, self);
        if (!mDefaultVibrator->setGlobalVibrationCallback(callback)) {
            // No active vibration in flight — close after delayMs without blocking.
            std::thread([delayMs, session = std::move(session), self = std::move(self)]() {
                if (delayMs > 0) {
                    usleep(static_cast<useconds_t>(delayMs) * 1000);
                }
                self->clearSession(session);
            }).detach();
        }
    }
}

void VibratorManager::clearSession(const std::shared_ptr<IVibrationSession>& session) {
    std::shared_ptr<IVibratorCallback> callback;
    {
        std::lock_guard lock(mMutex);
        if (mSession != session) {
            return;
        }
        callback = std::move(mSessionCallback);
        mSession = nullptr;
        mSessionCallback = nullptr;
    }
    if (callback) {
        std::thread([callback] { callback->onComplete(); }).detach();
    }
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
