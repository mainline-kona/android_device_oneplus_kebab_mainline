/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "KebabRichTapVibrator"

#include "Vibrator.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

namespace {

constexpr const char* kRichTapLib = "libaacvibrator.so";
constexpr const char* kDefaultDevicePath = "/dev/awinic_haptic";
constexpr unsigned int kRichTapGetF0Ioctl = _IO(0x52, 0x11);

constexpr int32_t kComposeDelayMaxMs = 1000;
constexpr int32_t kComposeSizeMax = 256;
constexpr float kDefaultQFactor = 18.0f;

// Prebaked RichTap effect IDs verified in libaacvibrator.so (aac_richtap.config)
constexpr uint32_t kEffectUltraShortTick = 0x300b;  // ~12 ms crisp tick
constexpr uint32_t kEffectLightClick     = 0x300a;  // ~20 ms crisp pop/light click
constexpr uint32_t kEffectStandardClick  = 0x3009;  // ~30 ms crisp click
constexpr uint32_t kEffectMediumClick    = 0x3008;  // ~40 ms crisp thud/medium
constexpr uint32_t kEffectHeavyClick     = 0x3007;  // ~50 ms crisp heavy click

}  // namespace

Vibrator::Vibrator() : mWorker(&Vibrator::workerLoop, this) {}

Vibrator::~Vibrator() {
    {
        std::lock_guard lock(mMutex);
        mShutdown = true;
        mSeq++;
        mHasTask = false;
        mPendingTask = nullptr;
    }
    mCv.notify_all();
    if (mWorker.joinable()) {
        mWorker.join();
    }
    if (mRichTap.ready && mRichTap.stop) {
        int32_t dummy = 0;
        mRichTap.stop(&dummy);
    }
    if (mRichTap.handle) {
        dlclose(mRichTap.handle);
        mRichTap.handle = nullptr;
    }
}

bool Vibrator::loadRichTap() {
    mRichTap.handle = dlopen(kRichTapLib, RTLD_NOW);
    if (!mRichTap.handle) {
        LOG(ERROR) << "Failed to dlopen " << kRichTapLib << ": " << dlerror();
        return false;
    }

    mRichTap.init = reinterpret_cast<int (*)(int32_t*)>(dlsym(mRichTap.handle, "aac_vibra_init"));
    mRichTap.on = reinterpret_cast<int (*)(uint32_t)>(dlsym(mRichTap.handle, "aac_vibra_on"));
    mRichTap.stop = reinterpret_cast<int (*)(int32_t*)>(dlsym(mRichTap.handle, "aac_vibra_stop"));
    mRichTap.setAmplitude =
            reinterpret_cast<int (*)(uint8_t)>(dlsym(mRichTap.handle, "aac_vibra_setAmplitude"));
    mRichTap.performId = reinterpret_cast<int (*)(uint32_t, int32_t)>(
            dlsym(mRichTap.handle, "aac_vibra_perform_id"));
    mRichTap.dynamicScale =
            reinterpret_cast<int (*)(uint8_t)>(dlsym(mRichTap.handle, "aac_vibra_dynamic_scale"));

    if (!mRichTap.init || !mRichTap.on || !mRichTap.stop || !mRichTap.setAmplitude ||
        !mRichTap.performId) {
        LOG(ERROR) << "Missing required symbols in " << kRichTapLib;
        dlclose(mRichTap.handle);
        mRichTap.handle = nullptr;
        return false;
    }

    int32_t initOut = 0;
    int rc = mRichTap.init(&initOut);
    if (rc != 0) {
        LOG(ERROR) << "aac_vibra_init() failed: " << rc;
        dlclose(mRichTap.handle);
        mRichTap.handle = nullptr;
        return false;
    }

    mRichTap.ready = true;
    LOG(INFO) << "AAC RichTap initialized cleanly (f0=" << mResonantFreqHz << " Hz)";
    return true;
}

float Vibrator::readResonantFrequencyHz() const {
    std::string devPath =
            ::android::base::GetProperty("vendor.aac_richtap.device.path", kDefaultDevicePath);
    auto semi = devPath.find(';');
    if (semi != std::string::npos) {
        devPath = devPath.substr(0, semi);
    }
    int fd = open(devPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return 170.0f;
    }
    uint32_t f0 = 0;
    float hz = 170.0f;
    if (ioctl(fd, kRichTapGetF0Ioctl, &f0) == 0 && f0 >= 1000 && f0 <= 3000) {
        hz = static_cast<float>(f0) / 10.0f;
    }
    close(fd);
    return hz;
}

bool Vibrator::init() {
    mResonantFreqHz = readResonantFrequencyHz();
    if (!loadRichTap()) {
        LOG(WARNING) << "RichTap unavailable; running vibrator HAL in no-op mode";
        return false;
    }
    mRichTap.setAmplitude(255);
    return true;
}

void Vibrator::stopActiveLocked() {
    mSeq++;
    mActive = false;
    mHasTask = false;
    mPendingTask = nullptr;
    mCv.notify_all();
    if (mRichTap.ready) {
        int32_t dummy = 0;
        mRichTap.stop(&dummy);
    }
}

void Vibrator::scheduleAsyncLocked(uint64_t seq, std::function<void(uint64_t)> task) {
    mActive = true;
    mPendingTask = std::move(task);
    mHasTask = true;
    mCv.notify_all();
    (void)seq;
}

bool Vibrator::waitCancellable(uint64_t seq, int32_t delayMs) {
    if (delayMs <= 0) {
        std::lock_guard lock(mMutex);
        return !mShutdown && mSeq == seq;
    }
    std::unique_lock lock(mMutex);
    return !mCv.wait_for(lock, std::chrono::milliseconds(delayMs),
                         [this, seq]() REQUIRES(mMutex) { return mShutdown || mSeq != seq; });
}

void Vibrator::finishCallback(uint64_t seq, const std::shared_ptr<IVibratorCallback>& callback) {
    std::shared_ptr<IVibratorCallback> globalCb;
    {
        std::lock_guard lock(mMutex);
        if (mShutdown || mSeq != seq) {
            return;
        }
        mActive = false;
        globalCb = mGlobalCallback;
        mGlobalCallback = nullptr;
    }
    if (callback) {
        callback->onComplete();
    }
    if (globalCb) {
        globalCb->onComplete();
    }
}

void Vibrator::workerLoop() {
    while (true) {
        std::function<void(uint64_t)> task;
        uint64_t seq = 0;
        {
            std::unique_lock lock(mMutex);
            mCv.wait(lock, [this]() REQUIRES(mMutex) { return mShutdown || mHasTask; });
            if (mShutdown) {
                return;
            }
            task = std::move(mPendingTask);
            mPendingTask = nullptr;
            mHasTask = false;
            seq = mSeq;
        }
        if (task) {
            task(seq);
        }
    }
}

int32_t Vibrator::strengthToAmplitude(EffectStrength strength) {
    switch (strength) {
        case EffectStrength::LIGHT:
            return 60;
        case EffectStrength::MEDIUM:
            return 85;
        case EffectStrength::STRONG:
            return 100;
        default:
            return -1;
    }
}

uint32_t Vibrator::primitiveToEffectId(CompositePrimitive primitive, int32_t* durationMs) {
    switch (primitive) {
        case CompositePrimitive::LIGHT_TICK:
            *durationMs = 12;
            return kEffectUltraShortTick;
        case CompositePrimitive::LOW_TICK:
            *durationMs = 15;
            return kEffectUltraShortTick;
        case CompositePrimitive::CLICK:
            *durationMs = 25;
            return kEffectStandardClick;
        case CompositePrimitive::THUD:
            *durationMs = 45;
            return kEffectHeavyClick;
        case CompositePrimitive::SPIN:
            *durationMs = 40;
            return kEffectMediumClick;
        case CompositePrimitive::QUICK_RISE:
        case CompositePrimitive::QUICK_FALL:
            *durationMs = 20;
            return kEffectLightClick;
        case CompositePrimitive::SLOW_RISE:
            *durationMs = 80;
            return 0;  // Uses synthetic equalized envelope via aac_vibra_on(80)
        default:
            *durationMs = 0;
            return UINT32_MAX;
    }
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS |
                    IVibrator::CAP_GET_RESONANT_FREQUENCY | IVibrator::CAP_GET_Q_FACTOR;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    std::shared_ptr<IVibratorCallback> globalCb;
    {
        std::lock_guard lock(mMutex);
        stopActiveLocked();
        globalCb = std::move(mGlobalCallback);
    }
    if (globalCb) {
        globalCb->onComplete();
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::on(int32_t timeoutMs,
                                const std::shared_ptr<IVibratorCallback>& callback) {
    if (timeoutMs <= 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard lock(mMutex);
    stopActiveLocked();
    uint64_t seq = mSeq;

    if (mRichTap.ready) {
        mRichTap.setAmplitude(mAmplitude);
        mRichTap.on(static_cast<uint32_t>(timeoutMs));
    }

    scheduleAsyncLocked(seq, [this, timeoutMs, callback](uint64_t mySeq) {
        if (waitCancellable(mySeq, timeoutMs)) {
            finishCallback(mySeq, callback);
        }
    });

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::perform(Effect effect, EffectStrength strength,
                                     const std::shared_ptr<IVibratorCallback>& callback,
                                     int32_t* _aidl_return) {
    int32_t amp = strengthToAmplitude(strength);
    if (amp < 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    uint32_t effectId = 0;
    int32_t durationMs = 0;
    bool isDoubleClick = false;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            effectId = kEffectUltraShortTick;
            durationMs = 12;
            break;
        case Effect::TICK:
            effectId = kEffectUltraShortTick;
            durationMs = 15;
            break;
        case Effect::POP:
            effectId = kEffectLightClick;
            durationMs = 20;
            break;
        case Effect::CLICK:
            effectId = kEffectStandardClick;
            durationMs = 30;
            break;
        case Effect::THUD:
            effectId = kEffectMediumClick;
            durationMs = 40;
            break;
        case Effect::HEAVY_CLICK:
            effectId = kEffectHeavyClick;
            durationMs = 50;
            break;
        case Effect::DOUBLE_CLICK:
            effectId = kEffectStandardClick;
            durationMs = 140;
            isDoubleClick = true;
            break;
        default:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    *_aidl_return = durationMs;

    std::lock_guard lock(mMutex);
    stopActiveLocked();
    uint64_t seq = mSeq;

    if (isDoubleClick) {
        scheduleAsyncLocked(seq, [this, effectId, amp, callback](uint64_t mySeq) {
            {
                std::lock_guard lock(mMutex);
                if (mShutdown || mSeq != mySeq) {
                    return;
                }
                if (mRichTap.ready) {
                    mRichTap.performId(effectId, amp);
                }
            }
            if (!waitCancellable(mySeq, 100)) {
                return;
            }
            {
                std::lock_guard lock(mMutex);
                if (mShutdown || mSeq != mySeq) {
                    return;
                }
                if (mRichTap.ready) {
                    mRichTap.performId(effectId, amp);
                }
            }
            if (waitCancellable(mySeq, 40)) {
                finishCallback(mySeq, callback);
            }
        });
    } else {
        if (mRichTap.ready) {
            mRichTap.performId(effectId, amp);
        }
        scheduleAsyncLocked(seq, [this, durationMs, callback](uint64_t mySeq) {
            if (waitCancellable(mySeq, durationMs)) {
                finishCallback(mySeq, callback);
            }
        });
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::performVendorEffect(
        const VendorEffect&, const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    *_aidl_return = {
            Effect::CLICK,
            Effect::DOUBLE_CLICK,
            Effect::TICK,
            Effect::THUD,
            Effect::POP,
            Effect::HEAVY_CLICK,
            Effect::TEXTURE_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setAmplitude(float amplitude) {
    if (std::isnan(amplitude) || amplitude <= 0.0f || amplitude > 1.0f) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    uint8_t scaled = static_cast<uint8_t>(std::clamp<int>(std::lround(amplitude * 255.0f), 1, 255));
    std::lock_guard lock(mMutex);
    mAmplitude = scaled;
    if (mRichTap.ready) {
        mRichTap.setAmplitude(scaled);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    *maxDelayMs = kComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    *maxSize = kComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    *supported = {
            CompositePrimitive::CLICK,
            CompositePrimitive::THUD,
            CompositePrimitive::SPIN,
            CompositePrimitive::QUICK_RISE,
            CompositePrimitive::SLOW_RISE,
            CompositePrimitive::QUICK_FALL,
            CompositePrimitive::LIGHT_TICK,
            CompositePrimitive::LOW_TICK,
    };
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getPrimitiveDuration(CompositePrimitive primitive,
                                                  int32_t* durationMs) {
    if (primitiveToEffectId(primitive, durationMs) == UINT32_MAX) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.empty() || static_cast<int32_t>(composite.size()) > kComposeSizeMax) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    for (const auto& item : composite) {
        if (item.delayMs < 0 || item.delayMs > kComposeDelayMaxMs || std::isnan(item.scale) ||
            item.scale < 0.0f || item.scale > 1.0f) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        int32_t dur = 0;
        if (item.primitive != CompositePrimitive::NOOP &&
            primitiveToEffectId(item.primitive, &dur) == UINT32_MAX) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    std::lock_guard lock(mMutex);
    stopActiveLocked();
    uint64_t seq = mSeq;

    scheduleAsyncLocked(seq, [this, composite, callback](uint64_t mySeq) {
        for (const auto& item : composite) {
            if (!waitCancellable(mySeq, item.delayMs)) {
                return;
            }
            if (item.primitive == CompositePrimitive::NOOP || item.scale <= 0.0f) {
                continue;
            }
            int32_t dur = 0;
            uint32_t id = primitiveToEffectId(item.primitive, &dur);
            {
                std::lock_guard lock(mMutex);
                if (mShutdown || mSeq != mySeq) {
                    return;
                }
                if (mRichTap.ready) {
                    if (id == 0) {
                        uint8_t amp = static_cast<uint8_t>(
                                std::clamp<int>(std::lround(item.scale * 255.0f), 1, 255));
                        mRichTap.setAmplitude(amp);
                        mRichTap.on(static_cast<uint32_t>(dur));
                    } else {
                        int32_t strength = std::clamp<int32_t>(
                                static_cast<int32_t>(std::lround(item.scale * 100.0f)), 1, 100);
                        mRichTap.performId(id, strength);
                    }
                }
            }
            if (!waitCancellable(mySeq, dur)) {
                return;
            }
        }
        finishCallback(mySeq, callback);
    });

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedAlwaysOnEffects(std::vector<Effect>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnEnable(int32_t, Effect, EffectStrength) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::alwaysOnDisable(int32_t) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getResonantFrequency(float* resonantFreqHz) {
    *resonantFreqHz = mResonantFreqHz;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    *qFactor = kDefaultQFactor;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getFrequencyResolution(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyMinimum(float*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getBandwidthAmplitudeMap(std::vector<float>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwlePrimitiveDurationMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleCompositionSizeMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedBraking(std::vector<Braking>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::composePwle(const std::vector<PrimitivePwle>&,
                                         const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getFrequencyToOutputAccelerationMap(
        std::vector<FrequencyAccelerationMapEntry>*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMaxMillis(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleV2PrimitiveDurationMinMillis(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getPwleV2CompositionSizeMax(int32_t*) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::composePwleV2(const CompositePwleV2&,
                                           const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

bool Vibrator::setGlobalVibrationCallback(const std::shared_ptr<IVibratorCallback>& callback) {
    std::lock_guard lock(mMutex);
    if (!mActive) {
        return false;
    }
    mGlobalCallback = callback;
    return true;
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
