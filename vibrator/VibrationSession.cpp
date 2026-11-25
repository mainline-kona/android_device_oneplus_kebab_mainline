/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "KebabRichTapVibrationSession"

#include "VibrationSession.h"

namespace aidl {
namespace android {
namespace hardware {
namespace vibrator {

static constexpr int32_t kSessionEndDelayMs = 20;

ndk::ScopedAStatus VibrationSession::close() {
    mManager->closeSession(kSessionEndDelayMs);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus VibrationSession::abort() {
    mManager->abortSession();
    return ndk::ScopedAStatus::ok();
}

}  // namespace vibrator
}  // namespace hardware
}  // namespace android
}  // namespace aidl
