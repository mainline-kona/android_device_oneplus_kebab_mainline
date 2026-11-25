/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/vibrator/BnVibrationSession.h>

#include "VibratorManager.h"

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

class VibrationSession : public BnVibrationSession {
  public:
    explicit VibrationSession(std::shared_ptr<VibratorManager> manager)
        : mManager(std::move(manager)) {}

    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus abort() override;

  private:
    std::shared_ptr<VibratorManager> mManager;
};

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
