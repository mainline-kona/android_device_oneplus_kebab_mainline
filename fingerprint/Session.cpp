/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic>
#include <chrono>
#include <errno.h>
#include <fcntl.h>
#include <format>
#include <string.h>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

#include <android-base/file.h>

#include "Legacy2Aidl.h"
#include "Session.h"

#include "CancellationSignal.h"

namespace aidl::android::hardware::biometrics::fingerprint {

namespace {

constexpr char kOplusHbmPath[] = "/sys/kernel/oplus_display/hbm";
constexpr char kOplusFpPressPath[] = "/sys/kernel/oplus_display/notify_fppress";
constexpr char kGoodixDevPath[] = "/dev/goodix_fp";

/* Matches drivers/input/fingerprint/gf_spi.h */
#define GF_IOC_MAGIC 'g'
#define GF_IOC_AUTO_SEND_TOUCHDOWN _IO(GF_IOC_MAGIC, 21)
#define GF_IOC_AUTO_SEND_TOUCHUP _IO(GF_IOC_MAGIC, 22)

constexpr int kOplusFingerprintEnumeratedType = 11;

struct oplus_fingerprint_enumerated_t {
    fingerprint_finger_id_t fingers[5];
    uint32_t count;
    uint32_t gid;
};

static_assert(offsetof(fingerprint_msg_t, data) == 8,
              "fingerprint_msg_t::data offset must match OPlus Goodix HAL (0x08)");
static_assert(offsetof(oplus_fingerprint_enumerated_t, count) == 0x28,
              "oplus_fingerprint_enumerated_t::count offset must be 0x28 (msg+0x30)");

std::atomic<bool> sFingerDown{false};
std::atomic<bool> sUiReady{false};
std::atomic<bool> sEnrolling{false};
std::atomic<uint64_t> sTouchSeq{0};

void sendGoodixTouchIoctl(bool down) {
    int fd = open(kGoodixDevPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        ALOGW("open(%s) failed: %s", kGoodixDevPath, strerror(errno));
        return;
    }
    unsigned long cmd = down ? GF_IOC_AUTO_SEND_TOUCHDOWN : GF_IOC_AUTO_SEND_TOUCHUP;
    if (ioctl(fd, cmd) < 0) {
        ALOGW("ioctl(%s, %s) failed: %s", kGoodixDevPath,
              down ? "TOUCHDOWN" : "TOUCHUP", strerror(errno));
    }
    close(fd);
}

void disableHbmAsync() {
    std::thread([]() {
        if (!sFingerDown.load() && !sEnrolling.load()) {
            ::android::base::WriteStringToFile("0", kOplusFpPressPath);
            ::android::base::WriteStringToFile("0", kOplusHbmPath);
        }
    }).detach();
}

void setEnrollHbm(bool enable) {
    bool prev = sEnrolling.exchange(enable);
    if (enable) {
        if (!prev) {
            ::android::base::WriteStringToFile("1", kOplusHbmPath);
        }
    } else if (prev && !sFingerDown.load()) {
        disableHbmAsync();
    }
}

void notifyUiReady() {
    if (!sFingerDown.load()) {
        return;
    }
    if (!sUiReady.exchange(true)) {
        ::android::base::WriteStringToFile("1", kOplusFpPressPath);
    }
}

void setFingerDown(bool down) {
    bool prev = sFingerDown.exchange(down);
    if (down == prev && !down) {
        return;
    }

    if (down) {
        sUiReady.store(false);
        /*
         * Enable HBM and notify Goodix TOUCHDOWN immediately on pointer down
         * so panel HBM settles in parallel with SystemUI presenting the UDFPS
         * illumination circle. Goodix capture (GF_NET_EVENT_UI_READY) waits
         * for onUiReady() so frame #1 is never taken on a dark/unlit dot.
         */
        if (!sEnrolling.load()) {
            ::android::base::WriteStringToFile("1", kOplusHbmPath);
        }
        sendGoodixTouchIoctl(true);
        /* Fallback if a caller never invokes onUiReady() after onPointerDown() */
        uint64_t seq = ++sTouchSeq;
        std::thread([seq]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(60));
            if (sTouchSeq.load() == seq && sFingerDown.load() && !sUiReady.load()) {
                notifyUiReady();
            }
        }).detach();
    } else {
        ++sTouchSeq;
        bool hadUiReady = sUiReady.exchange(false);
        sendGoodixTouchIoctl(false);
        if (!sEnrolling.load() && (hadUiReady || prev)) {
            disableHbmAsync();
        }
    }
}

}  // namespace

void onClientDeath(void* cookie) {
    ALOGI("FingerprintService has died");
    Session* session = static_cast<Session*>(cookie);
    if (session && !session->isClosed()) {
        session->close();
    }
}

Session::Session(fingerprint_device_t* device, int userId, std::shared_ptr<ISessionCallback> cb,
                 LockoutTracker lockoutTracker)
    : mDevice(device), mLockoutTracker(lockoutTracker), mUserId(userId), mCb(cb) {
    mDeathRecipient = AIBinder_DeathRecipient_new(onClientDeath);

    auto path = std::format("/data/vendor_de/{}/fpdata/", userId);
    mDevice->set_active_group(mDevice, mUserId, path.c_str());
}

ndk::ScopedAStatus Session::generateChallenge() {
    uint64_t challenge = mDevice->pre_enroll(mDevice);
    ALOGI("generateChallenge: %ld", challenge);
    mCb->onChallengeGenerated(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    ALOGI("revokeChallenge: %ld", challenge);
    setFingerDown(false);
    setEnrollHbm(false);
    mDevice->post_enroll(mDevice);
    mCb->onChallengeRevoked(challenge);

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                                   std::shared_ptr<ICancellationSignal>* out) {
    hw_auth_token_t authToken{};
    translate(hat, authToken);
    setEnrollHbm(true);
    int error = mDevice->enroll(mDevice, &authToken, mUserId, 60);
    if (error) {
        ALOGE("enroll failed: %d", error);
        setEnrollHbm(false);
        mCb->onError(Error::UNABLE_TO_PROCESS, error);
    }

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticate(int64_t operationId,
                                         std::shared_ptr<ICancellationSignal>* out) {
    checkSensorLockout();
    int error = mDevice->authenticate(mDevice, operationId, mUserId);
    if (error) {
        ALOGE("authenticate failed: %d", error);
        mCb->onError(Error::UNABLE_TO_PROCESS, error);
    }

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    ALOGD("Detect interaction is not supported");
    mCb->onError(Error::UNABLE_TO_PROCESS, 0 /* vendorCode */);

    *out = SharedRefBase::make<CancellationSignal>(this);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::enumerateEnrollments() {
    {
        std::lock_guard<std::mutex> lock(mEnumerateMutex);
        mEnumerateDone = false;
        mEnrollments.clear();
    }
    int error = mDevice->enumerate(mDevice);
    if (error) {
        ALOGE("enumerate failed: %d", error);
    }

    /*
     * Goodix processes set_active_group() and enumerate() asynchronously on its
     * EventCenter thread. Wait for FINGERPRINT_TEMPLATE_ENUMERATING (remaining==0)
     * before assuming 0 templates exist; otherwise firing an empty list early
     * causes FingerprintService to purge valid enrolled fingerprints as dangling.
     */
    bool timedOut = false;
    {
        std::unique_lock<std::mutex> lock(mEnumerateMutex);
        if (error != 0) {
            timedOut = !mEnumerateDone;
            mEnumerateDone = true;
        } else {
            bool done = mEnumerateCv.wait_for(lock, std::chrono::milliseconds(1500),
                                              [this]() { return mEnumerateDone; });
            if (!done) {
                timedOut = true;
                mEnumerateDone = true;
            }
        }
    }
    if (timedOut) {
        ALOGI("enumerate produced no templates within timeout; notifying empty list");
        mCb->onEnrollmentsEnumerated({});
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& enrollmentIds) {
    ALOGI("removeEnrollments, size: %zu", enrollmentIds.size());

    for (int32_t fid : enrollmentIds) {
        int error = mDevice->remove(mDevice, mUserId, fid);
        if (error) {
            ALOGE("remove failed: %d", error);
        }
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::getAuthenticatorId() {
    uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
    ALOGI("getAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdRetrieved(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::invalidateAuthenticatorId() {
    uint64_t auth_id = mDevice->get_authenticator_id(mDevice);
    ALOGI("invalidateAuthenticatorId: %ld", auth_id);
    mCb->onAuthenticatorIdInvalidated(auth_id);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::resetLockout(const HardwareAuthToken& /*hat*/) {
    clearLockout(true);
    if (mIsLockoutTimerStarted) mIsLockoutTimerAborted = true;

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerDown(int32_t /*pointerId*/, int32_t /*x*/, int32_t /*y*/,
                                          float /*minor*/, float /*major*/) {
    setFingerDown(true);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerUp(int32_t /*pointerId*/) {
    setFingerDown(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onUiReady() {
    notifyUiReady();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::authenticateWithContext(
        int64_t operationId, const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return authenticate(operationId, out);
}

ndk::ScopedAStatus Session::enrollWithContext(const keymaster::HardwareAuthToken& hat,
                                              const common::OperationContext& /*context*/,
                                              std::shared_ptr<common::ICancellationSignal>* out) {
    return enroll(hat, out);
}

ndk::ScopedAStatus Session::detectInteractionWithContext(
        const common::OperationContext& /*context*/,
        std::shared_ptr<common::ICancellationSignal>* out) {
    return detectInteraction(out);
}

ndk::ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}

ndk::ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}

ndk::ScopedAStatus Session::onContextChanged(const common::OperationContext& /*context*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& /*context*/) {
    setFingerDown(false);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::setIgnoreDisplayTouches(bool /*shouldIgnore*/) {
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Session::cancel() {
    setFingerDown(false);
    setEnrollHbm(false);
    int ret = mDevice->cancel(mDevice);

    if (ret == 0) {
        mCb->onError(Error::CANCELED, 0 /* vendorCode */);
        return ndk::ScopedAStatus::ok();
    }

    return ndk::ScopedAStatus::fromServiceSpecificError(ret);
}

ndk::ScopedAStatus Session::close() {
    setFingerDown(false);
    setEnrollHbm(false);
    mClosed = true;
    mCb->onSessionClosed();
    AIBinder_DeathRecipient_delete(mDeathRecipient);
    return ndk::ScopedAStatus::ok();
}

binder_status_t Session::linkToDeath(AIBinder* binder) {
    return AIBinder_linkToDeath(binder, mDeathRecipient, this);
}

bool Session::isClosed() {
    return mClosed;
}

Error Session::VendorErrorFilter(int32_t error, int32_t* vendorCode) {
    *vendorCode = 0;

    switch (error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return Error::HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return Error::UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return Error::TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return Error::NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return Error::CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return Error::UNABLE_TO_REMOVE;
        case FINGERPRINT_ERROR_LOCKOUT: {
            *vendorCode = FINGERPRINT_ERROR_LOCKOUT;
            return Error::VENDOR;
        }
        default:
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                *vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return Error::VENDOR;
            }
    }
    ALOGE("Unknown error from fingerprint vendor library: %d", error);
    return Error::UNABLE_TO_PROCESS;
}

AcquiredInfo Session::VendorAcquiredFilter(int32_t info, int32_t* vendorCode) {
    *vendorCode = 0;

    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return AcquiredInfo::GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return AcquiredInfo::PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return AcquiredInfo::INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return AcquiredInfo::SENSOR_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return AcquiredInfo::TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return AcquiredInfo::TOO_FAST;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                *vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return AcquiredInfo::VENDOR;
            }
    }
    ALOGE("Unknown acquired message from fingerprint vendor library: %d", info);
    return AcquiredInfo::UNKNOWN;
}

bool Session::checkSensorLockout() {
    LockoutTracker::LockoutMode lockoutMode = mLockoutTracker.getMode();
    if (lockoutMode == LockoutTracker::LockoutMode::kPermanent) {
        ALOGE("Fail: lockout permanent");
        mCb->onLockoutPermanent();
        mIsLockoutTimerAborted = true;
        return true;
    }
    if (lockoutMode == LockoutTracker::LockoutMode::kTimed) {
        int64_t timeLeft = mLockoutTracker.getLockoutTimeLeft();
        ALOGE("Fail: lockout timed: %ld", timeLeft);
        mCb->onLockoutTimed(timeLeft);
        if (!mIsLockoutTimerStarted) startLockoutTimer(timeLeft);
        return true;
    }
    return false;
}

void Session::clearLockout(bool clearAttemptCounter) {
    mLockoutTracker.reset(clearAttemptCounter);
    mCb->onLockoutCleared();
}

void Session::startLockoutTimer(int64_t timeout) {
    std::function<void()> action = std::bind(&Session::lockoutTimerExpired, this);
    std::thread([timeout, action]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(timeout));
        action();
    }).detach();

    mIsLockoutTimerStarted = true;
}

void Session::lockoutTimerExpired() {
    if (!mIsLockoutTimerAborted) clearLockout(false);

    mIsLockoutTimerStarted = false;
    mIsLockoutTimerAborted = false;
}

void Session::notify(const fingerprint_msg_t* msg) {
    switch (static_cast<int>(msg->type)) {
        case FINGERPRINT_ERROR: {
            int32_t vendorCode = 0;
            Error result = VendorErrorFilter(msg->data.error, &vendorCode);
            ALOGD("onError(%hhd, %d)", result, vendorCode);
            mCb->onError(result, vendorCode);
            setFingerDown(false);
            setEnrollHbm(false);
        } break;
        case FINGERPRINT_ACQUIRED: {
            int32_t vendorCode = 0;
            AcquiredInfo result =
                    VendorAcquiredFilter(msg->data.acquired.acquired_info, &vendorCode);
            ALOGD("onAcquired(%hhd, %d)", result, vendorCode);
            // Goodix reports finger down/up as vendor codes, which would make
            // SystemUI drop the UDFPS illumination mid-capture.
            if (result != AcquiredInfo::VENDOR) {
                mCb->onAcquired(result, vendorCode);
            }
        } break;
        case FINGERPRINT_TEMPLATE_ENROLLING: {
            ALOGD("onEnrollResult(fid=%d, gid=%d, rem=%d)", msg->data.enroll.finger.fid,
                  msg->data.enroll.finger.gid, msg->data.enroll.samples_remaining);
            mCb->onEnrollmentProgress(msg->data.enroll.finger.fid,
                                      msg->data.enroll.samples_remaining);
            if (msg->data.enroll.samples_remaining == 0) {
                setFingerDown(false);
                setEnrollHbm(false);
            }
        } break;
        case FINGERPRINT_TEMPLATE_REMOVED: {
            ALOGD("onRemove(fid=%d, gid=%d, rem=%d)", msg->data.removed.finger.fid,
                  msg->data.removed.finger.gid, msg->data.removed.remaining_templates);
            std::vector<int> enrollments;
            enrollments.push_back(msg->data.removed.finger.fid);
            mCb->onEnrollmentsRemoved(enrollments);
        } break;
        case FINGERPRINT_AUTHENTICATED: {
            ALOGD("onAuthenticated(fid=%d, gid=%d)", msg->data.authenticated.finger.fid,
                  msg->data.authenticated.finger.gid);
            if (msg->data.authenticated.finger.fid != 0) {
                const hw_auth_token_t hat = msg->data.authenticated.hat;
                HardwareAuthToken authToken;
                translate(hat, authToken);

                mCb->onAuthenticationSucceeded(msg->data.authenticated.finger.fid, authToken);
                mLockoutTracker.reset(true);
            } else {
                mCb->onAuthenticationFailed();
                mLockoutTracker.addFailedAttempt();
                checkSensorLockout();
            }
            setFingerDown(false);
        } break;
        case FINGERPRINT_TEMPLATE_ENUMERATING: {
            ALOGD("onEnumerate(fid=%d, gid=%d, rem=%d)", msg->data.enumerated.finger.fid,
                  msg->data.enumerated.finger.gid, msg->data.enumerated.remaining_templates);
            std::vector<int32_t> completed;
            bool shouldNotify = false;
            {
                std::lock_guard<std::mutex> lock(mEnumerateMutex);
                if (!mEnumerateDone) {
                    if (msg->data.enumerated.finger.fid != 0) {
                        mEnrollments.push_back(msg->data.enumerated.finger.fid);
                    }
                    if (msg->data.enumerated.remaining_templates == 0) {
                        completed.swap(mEnrollments);
                        mEnumerateDone = true;
                        shouldNotify = true;
                    }
                }
            }
            if (shouldNotify) {
                mCb->onEnrollmentsEnumerated(completed);
                mEnumerateCv.notify_all();
            }
        } break;
        case kOplusFingerprintEnumeratedType: {
            /*
             * OPlus Goodix HALs (19805 G6_3/G6_7) send all enrolled templates in a
             * single batch callback with msg->type == 11 instead of standard AOSP
             * FINGERPRINT_TEMPLATE_ENUMERATING (6).
             */
            const auto* oplusEnum =
                    reinterpret_cast<const oplus_fingerprint_enumerated_t*>(&msg->data);
            uint32_t count = oplusEnum->count;
            if (count > 5) count = 5;
            std::vector<int32_t> completed;
            bool shouldNotify = false;
            {
                std::lock_guard<std::mutex> lock(mEnumerateMutex);
                if (!mEnumerateDone) {
                    for (uint32_t i = 0; i < count; ++i) {
                        uint32_t fid = oplusEnum->fingers[i].fid;
                        uint32_t gid = oplusEnum->fingers[i].gid;
                        ALOGI("onOplusEnumerate[%u/%u](fid=%u, gid=%u)", i, count, fid, gid);
                        if (fid != 0) {
                            completed.push_back(static_cast<int32_t>(fid));
                        }
                    }
                    mEnrollments.clear();
                    mEnumerateDone = true;
                    shouldNotify = true;
                }
            }
            if (shouldNotify) {
                ALOGI("onOplusEnumerate: reporting %zu enrolled template(s)", completed.size());
                mCb->onEnrollmentsEnumerated(completed);
                mEnumerateCv.notify_all();
            }
        } break;
        default: {
            ALOGW("Unhandled fingerprint_msg_t type=%d", static_cast<int>(msg->type));
        } break;
    }
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
