/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibrator.h>
#include <android-base/thread_annotations.h>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class Vibrator : public BnVibrator {
  public:
    Vibrator();
    ~Vibrator() override;

    bool init();

    ndk::ScopedAStatus getCapabilities(int32_t* _aidl_return) override;
    ndk::ScopedAStatus off() override;
    ndk::ScopedAStatus on(int32_t timeoutMs,
                          const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus perform(Effect effect, EffectStrength strength,
                               const std::shared_ptr<IVibratorCallback>& callback,
                               int32_t* _aidl_return) override;
    ndk::ScopedAStatus performVendorEffect(
            const VendorEffect& effect,
            const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus setAmplitude(float amplitude) override;
    ndk::ScopedAStatus setExternalControl(bool enabled) override;
    ndk::ScopedAStatus getCompositionDelayMax(int32_t* maxDelayMs) override;
    ndk::ScopedAStatus getCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedPrimitives(std::vector<CompositePrimitive>* supported) override;
    ndk::ScopedAStatus getPrimitiveDuration(CompositePrimitive primitive,
                                            int32_t* durationMs) override;
    ndk::ScopedAStatus compose(const std::vector<CompositeEffect>& composite,
                               const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getSupportedAlwaysOnEffects(std::vector<Effect>* _aidl_return) override;
    ndk::ScopedAStatus alwaysOnEnable(int32_t id, Effect effect, EffectStrength strength) override;
    ndk::ScopedAStatus alwaysOnDisable(int32_t id) override;
    ndk::ScopedAStatus getResonantFrequency(float* resonantFreqHz) override;
    ndk::ScopedAStatus getQFactor(float* qFactor) override;
    ndk::ScopedAStatus getFrequencyResolution(float* freqResolutionHz) override;
    ndk::ScopedAStatus getFrequencyMinimum(float* freqMinimumHz) override;
    ndk::ScopedAStatus getBandwidthAmplitudeMap(std::vector<float>* _aidl_return) override;
    ndk::ScopedAStatus getPwlePrimitiveDurationMax(int32_t* durationMs) override;
    ndk::ScopedAStatus getPwleCompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus getSupportedBraking(std::vector<Braking>* supported) override;
    ndk::ScopedAStatus composePwle(const std::vector<PrimitivePwle>& composite,
                                   const std::shared_ptr<IVibratorCallback>& callback) override;
    ndk::ScopedAStatus getFrequencyToOutputAccelerationMap(
            std::vector<FrequencyAccelerationMapEntry>* _aidl_return) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMaxMillis(int32_t* maxDurationMs) override;
    ndk::ScopedAStatus getPwleV2PrimitiveDurationMinMillis(int32_t* minDurationMs) override;
    ndk::ScopedAStatus getPwleV2CompositionSizeMax(int32_t* maxSize) override;
    ndk::ScopedAStatus composePwleV2(const CompositePwleV2& composite,
                                     const std::shared_ptr<IVibratorCallback>& callback) override;

    void setGlobalVibrationCallback(const std::shared_ptr<IVibratorCallback>& callback);

  private:
    struct RichTapApi {
        void* handle = nullptr;
        int (*init)(int32_t* out) = nullptr;
        int (*on)(uint32_t timeoutMs) = nullptr;
        int (*stop)(int32_t* out) = nullptr;
        int (*setAmplitude)(uint8_t amplitude) = nullptr;
        int (*performId)(uint32_t effectId, int32_t amplitude) = nullptr;
        int (*dynamicScale)(uint8_t scale) = nullptr;
        bool ready = false;
    };

    bool loadRichTap();
    float readResonantFrequencyHz() const;
    void stopActiveLocked() REQUIRES(mMutex);
    void scheduleAsyncLocked(uint64_t seq, std::function<void(uint64_t)> task) REQUIRES(mMutex);
    bool waitCancellable(uint64_t seq, int32_t delayMs);
    void finishCallback(uint64_t seq, const std::shared_ptr<IVibratorCallback>& callback);
    void workerLoop();

    static int32_t strengthToAmplitude(EffectStrength strength);
    static uint32_t primitiveToEffectId(CompositePrimitive primitive, int32_t* durationMs);

    RichTapApi mRichTap;
    float mResonantFreqHz = 170.0f;

    mutable std::mutex mMutex;
    std::condition_variable mCv;
    uint64_t mSeq GUARDED_BY(mMutex) = 0;
    bool mShutdown GUARDED_BY(mMutex) = false;
    bool mHasTask GUARDED_BY(mMutex) = false;
    std::function<void(uint64_t)> mPendingTask GUARDED_BY(mMutex);
    uint8_t mAmplitude GUARDED_BY(mMutex) = 255;
    std::shared_ptr<IVibratorCallback> mGlobalCallback GUARDED_BY(mMutex);
    std::thread mWorker;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
