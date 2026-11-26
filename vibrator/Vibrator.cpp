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

constexpr int32_t kComposeDelayMaxMs = 5000;
constexpr int32_t kComposeSizeMax = 256;

// Margin added before firing IVibratorCallback::onComplete() for short RTP
// bursts so VibratorManagerService's trailing off() -> RICHTAP_STOP_MODE does
// not cut off the waveform's active braking tail while the looper/I2C RTP
// transfer (~8-12 ms) is still finishing on the AW8697.
constexpr int32_t kRtpPipelineMarginMs = 15;

// libaacvibrator.so maps strength (<70 -> L0 @ 0.50 peak, 70..89 -> L1 @ 0.80
// peak, 90..100 -> L2 @ 1.00 peak) with unity table gain at 50 / 80 / 100.
constexpr int32_t kRichTapLightStrength = 50;
constexpr int32_t kRichTapMediumStrength = 80;
constexpr int32_t kRichTapStrongStrength = 100;

// Stock OnePlus 8T aac_richtap.config effect IDs (3 prebaked 0x1000..0x1002
// and 11 JND transients 0x3001..0x300b), matching hardware/oplus RichTap HAL.
constexpr uint32_t kRichTapPrebakedDoubleClick = 0x1001;  // 85 ms
constexpr uint32_t kRichTapJndThud             = 0x3001;  // 36 ms
constexpr uint32_t kRichTapJndHeavyClick       = 0x3002;  // 30 ms
constexpr uint32_t kRichTapJndPop              = 0x3004;  // 24 ms
constexpr uint32_t kRichTapJndClick            = 0x3009;  // 12 ms
constexpr uint32_t kRichTapJndTick             = 0x300a;  // 12 ms
constexpr uint32_t kRichTapJndLightTick        = 0x300b;  // 11 ms

uint8_t floatToGainByte(float amplitude) {
    return static_cast<uint8_t>(std::clamp<int>(std::lround(amplitude * 255.0f), 1, 255));
}

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
        int32_t stopState = -1;
        mRichTap.stop(&stopState);
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
    mRichTap.looperStart =
            reinterpret_cast<void (*)()>(dlsym(mRichTap.handle, "_Z22aac_vibra_looper_startv"));
    mRichTap.looperOn = reinterpret_cast<int32_t (*)(uint32_t)>(
            dlsym(mRichTap.handle, "_Z19aac_vibra_looper_onj"));
    mRichTap.looperPrebakedEffect = reinterpret_cast<int32_t (*)(uint32_t, int32_t)>(
            dlsym(mRichTap.handle, "_Z32aac_vibra_looper_prebaked_effectji"));
    mRichTap.on = reinterpret_cast<int (*)(uint32_t)>(dlsym(mRichTap.handle, "aac_vibra_on"));
    mRichTap.stop = reinterpret_cast<int (*)(int32_t*)>(dlsym(mRichTap.handle, "aac_vibra_stop"));
    mRichTap.setAmplitude =
            reinterpret_cast<int (*)(uint8_t)>(dlsym(mRichTap.handle, "aac_vibra_setAmplitude"));
    mRichTap.performId = reinterpret_cast<int (*)(uint32_t, int32_t)>(
            dlsym(mRichTap.handle, "aac_vibra_perform_id"));
    mRichTap.dynamicScale =
            reinterpret_cast<int (*)(uint8_t)>(dlsym(mRichTap.handle, "aac_vibra_dynamic_scale"));

    if (!mRichTap.init || !mRichTap.stop || !mRichTap.setAmplitude ||
        (!mRichTap.looperPrebakedEffect && !mRichTap.performId)) {
        LOG(ERROR) << "Missing required symbols in " << kRichTapLib;
        dlclose(mRichTap.handle);
        mRichTap.handle = nullptr;
        return false;
    }

    int32_t deviceType = 0;
    int rc = mRichTap.init(&deviceType);
    if (rc != 0) {
        LOG(ERROR) << "aac_vibra_init() failed: " << rc;
        dlclose(mRichTap.handle);
        mRichTap.handle = nullptr;
        return false;
    }

    if (mRichTap.looperStart) {
        mRichTap.looperStart();
    } else {
        LOG(WARNING) << "aac_vibra_looper_start not found; falling back to direct perform API";
    }

    mRichTap.ready = true;
    LOG(INFO) << "AAC RichTap initialized cleanly (deviceType=" << deviceType
              << ", f0=" << mResonantFreqHz
              << " Hz, looper=" << (mRichTap.looperStart != nullptr) << ")";
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
    mRichTap.setAmplitude(0xff);
    return true;
}

void Vibrator::cancelPendingLocked() {
    mSeq++;
    mActive = false;
    mHasTask = false;
    mPendingTask = nullptr;
    mCv.notify_all();
}

void Vibrator::beginVibrationLocked() {
    mPeakAmplitude = mCurrentAmplitude;
    mAmplitudeSetSinceStart = false;
    mAmplitudeSetForEffect = false;
    if (mRichTap.ready) {
        mRichTap.setAmplitude(floatToGainByte(mCurrentAmplitude));
    }
}

void Vibrator::endVibrationLocked() {
    mAmplitudeSetForEffect = false;
    if (!mAmplitudeSetSinceStart) {
        return;
    }
    mCurrentAmplitude = mPeakAmplitude;
    mAmplitudeSetSinceStart = false;
}

void Vibrator::applyFullAmplitudeLocked() {
    mAmplitudeSetForEffect = false;
    if (mRichTap.ready) {
        mRichTap.setAmplitude(0xff);
    }
}

int32_t Vibrator::playPrebakedLocked(uint32_t effectId, int32_t strength, int32_t fallbackMs) {
    if (!mRichTap.ready) {
        return fallbackMs;
    }
    if (mRichTap.looperPrebakedEffect) {
        int32_t dur = mRichTap.looperPrebakedEffect(effectId, strength);
        if (dur > 0) {
            return dur;
        }
        LOG(WARNING) << "aac_vibra_looper_prebaked_effect(0x" << std::hex << effectId << std::dec
                     << ", " << strength << ") returned " << dur;
    }
    if (mRichTap.performId) {
        mRichTap.performId(effectId, strength);
    }
    return fallbackMs;
}

int32_t Vibrator::playOnLocked(uint32_t timeoutMs) {
    if (!mRichTap.ready) {
        return static_cast<int32_t>(timeoutMs);
    }
    if (mRichTap.looperOn) {
        int32_t dur = mRichTap.looperOn(timeoutMs);
        if (dur > 0) {
            return dur;
        }
        LOG(WARNING) << "aac_vibra_looper_on(" << timeoutMs << ") returned " << dur;
    }
    if (mRichTap.on) {
        mRichTap.on(timeoutMs);
    }
    return static_cast<int32_t>(timeoutMs);
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

void Vibrator::finishCallback(uint64_t seq, const std::shared_ptr<IVibratorCallback>& callback,
                              bool restoreAmplitude) {
    std::shared_ptr<IVibratorCallback> globalCb;
    {
        std::lock_guard lock(mMutex);
        if (mShutdown || mSeq != seq) {
            return;
        }
        if (restoreAmplitude) {
            endVibrationLocked();
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
            return kRichTapLightStrength;
        case EffectStrength::MEDIUM:
            return kRichTapMediumStrength;
        case EffectStrength::STRONG:
            return kRichTapStrongStrength;
        default:
            return -1;
    }
}

uint32_t Vibrator::primitiveToEffectId(CompositePrimitive primitive, int32_t* durationMs) {
    switch (primitive) {
        case CompositePrimitive::NOOP:
            *durationMs = 0;
            return 0;
        case CompositePrimitive::LIGHT_TICK:
            *durationMs = 11;
            return kRichTapJndLightTick;
        case CompositePrimitive::LOW_TICK:
            *durationMs = 12;
            return kRichTapJndTick;
        case CompositePrimitive::CLICK:
            *durationMs = 12;
            return kRichTapJndClick;
        case CompositePrimitive::QUICK_RISE:
            *durationMs = 12;
            return kRichTapJndClick;
        case CompositePrimitive::QUICK_FALL:
            *durationMs = 12;
            return kRichTapJndTick;
        case CompositePrimitive::SPIN:
            *durationMs = 24;
            return kRichTapJndPop;
        case CompositePrimitive::SLOW_RISE:
            *durationMs = 30;
            return kRichTapJndHeavyClick;
        case CompositePrimitive::THUD:
            *durationMs = 36;
            return kRichTapJndThud;
        default:
            *durationMs = 0;
            return UINT32_MAX;
    }
}

ndk::ScopedAStatus Vibrator::getCapabilities(int32_t* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *_aidl_return = IVibrator::CAP_ON_CALLBACK | IVibrator::CAP_PERFORM_CALLBACK |
                    IVibrator::CAP_AMPLITUDE_CONTROL | IVibrator::CAP_COMPOSE_EFFECTS |
                    IVibrator::CAP_GET_RESONANT_FREQUENCY;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::off() {
    std::shared_ptr<IVibratorCallback> globalCb;
    {
        std::lock_guard lock(mMutex);
        cancelPendingLocked();
        if (mRichTap.ready && mRichTap.stop) {
            int32_t stopState = -1;
            mRichTap.stop(&stopState);
        }
        endVibrationLocked();
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
    cancelPendingLocked();
    uint64_t seq = mSeq;

    beginVibrationLocked();
    int32_t durMs = playOnLocked(static_cast<uint32_t>(timeoutMs));
    int32_t waitMs = durMs + (durMs < 56 ? kRtpPipelineMarginMs : 0);

    scheduleAsyncLocked(seq, [this, waitMs, callback](uint64_t mySeq) {
        if (waitCancellable(mySeq, waitMs)) {
            finishCallback(mySeq, callback, /*restoreAmplitude=*/true);
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
    int32_t fallbackDurationMs = 0;

    switch (effect) {
        case Effect::TEXTURE_TICK:
            effectId = kRichTapJndLightTick;
            fallbackDurationMs = 11;
            break;
        case Effect::TICK:
            effectId = kRichTapJndTick;
            fallbackDurationMs = 12;
            break;
        case Effect::CLICK:
            effectId = kRichTapJndClick;
            fallbackDurationMs = 12;
            break;
        case Effect::DOUBLE_CLICK:
            effectId = kRichTapPrebakedDoubleClick;
            fallbackDurationMs = 85;
            break;
        case Effect::POP:
            effectId = kRichTapJndPop;
            fallbackDurationMs = 24;
            break;
        case Effect::THUD:
            effectId = kRichTapJndThud;
            fallbackDurationMs = 36;
            break;
        case Effect::HEAVY_CLICK:
            effectId = kRichTapJndHeavyClick;
            fallbackDurationMs = 30;
            break;
        default:
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    std::lock_guard lock(mMutex);
    cancelPendingLocked();
    uint64_t seq = mSeq;

    if (mAmplitudeSetForEffect) {
        beginVibrationLocked();
    } else {
        applyFullAmplitudeLocked();
    }

    int32_t durationMs = playPrebakedLocked(effectId, amp, fallbackDurationMs);
    if (_aidl_return != nullptr) {
        *_aidl_return = durationMs;
    }

    int32_t waitMs = durationMs + kRtpPipelineMarginMs;
    scheduleAsyncLocked(seq, [this, waitMs, callback](uint64_t mySeq) {
        if (waitCancellable(mySeq, waitMs)) {
            finishCallback(mySeq, callback);
        }
    });

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::performVendorEffect(
        const VendorEffect&, const std::shared_ptr<IVibratorCallback>&) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getSupportedEffects(std::vector<Effect>* _aidl_return) {
    if (_aidl_return == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
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
    if (!(amplitude > 0.0f && amplitude <= 1.0f)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    std::lock_guard lock(mMutex);
    mCurrentAmplitude = amplitude;
    mAmplitudeSetForEffect = true;
    if (!mAmplitudeSetSinceStart) {
        mPeakAmplitude = amplitude;
        mAmplitudeSetSinceStart = true;
    } else {
        mPeakAmplitude = std::max(mPeakAmplitude, amplitude);
    }
    if (mRichTap.ready) {
        mRichTap.setAmplitude(floatToGainByte(amplitude));
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::setExternalControl(bool) {
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ndk::ScopedAStatus Vibrator::getCompositionDelayMax(int32_t* maxDelayMs) {
    if (maxDelayMs == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *maxDelayMs = kComposeDelayMaxMs;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getCompositionSizeMax(int32_t* maxSize) {
    if (maxSize == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *maxSize = kComposeSizeMax;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getSupportedPrimitives(std::vector<CompositePrimitive>* supported) {
    if (supported == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *supported = {
            CompositePrimitive::NOOP,
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
    if (durationMs == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    if (primitiveToEffectId(primitive, durationMs) == UINT32_MAX) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::compose(const std::vector<CompositeEffect>& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) {
    if (composite.empty() || composite.size() > static_cast<size_t>(kComposeSizeMax)) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }

    for (const auto& item : composite) {
        if (item.delayMs < 0 || item.delayMs > kComposeDelayMaxMs) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        if (!(item.scale >= 0.0f && item.scale <= 1.0f)) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        int32_t dur = 0;
        if (primitiveToEffectId(item.primitive, &dur) == UINT32_MAX) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
        }
    }

    std::lock_guard lock(mMutex);
    cancelPendingLocked();
    uint64_t seq = mSeq;

    scheduleAsyncLocked(seq, [this, composite, callback](uint64_t mySeq) {
        {
            std::lock_guard lock(mMutex);
            if (mShutdown || mSeq != mySeq) {
                return;
            }
            applyFullAmplitudeLocked();
        }

        bool playedAny = false;
        for (const auto& item : composite) {
            if (item.delayMs > 0) {
                if (!waitCancellable(mySeq, item.delayMs)) {
                    return;
                }
            }
            if (item.primitive == CompositePrimitive::NOOP) {
                continue;
            }
            int32_t fallbackDur = 0;
            uint32_t id = primitiveToEffectId(item.primitive, &fallbackDur);
            if (item.scale == 0.0f) {
                if (!waitCancellable(mySeq, fallbackDur)) {
                    return;
                }
                continue;
            }
            int32_t dur = fallbackDur;
            {
                std::lock_guard lock(mMutex);
                if (mShutdown || mSeq != mySeq) {
                    return;
                }
                int32_t strength = std::clamp<int32_t>(
                        static_cast<int32_t>(std::lround(item.scale * 100.0f)), 1, 100);
                dur = playPrebakedLocked(id, strength, fallbackDur);
                playedAny = true;
            }
            if (!waitCancellable(mySeq, dur)) {
                return;
            }
        }
        if (playedAny && !waitCancellable(mySeq, kRtpPipelineMarginMs)) {
            return;
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
    if (resonantFreqHz == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    *resonantFreqHz = mResonantFreqHz;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Vibrator::getQFactor(float* qFactor) {
    if (qFactor == nullptr) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
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
