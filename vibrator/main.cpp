/*
 * SPDX-FileCopyrightText: The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "KebabRichTapVibratorMain"

#include "Vibrator.h"
#include "VibratorManager.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using aidl::android::hardware::vibrator::Vibrator;
using aidl::android::hardware::vibrator::VibratorManager;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);

    auto vib = ndk::SharedRefBase::make<Vibrator>();
    vib->init();

    binder_status_t status = AServiceManager_addService(
            vib->asBinder().get(), Vibrator::makeServiceName("default").c_str());
    CHECK_EQ(status, STATUS_OK);

    auto vibManager = ndk::SharedRefBase::make<VibratorManager>(vib);
    status = AServiceManager_addService(vibManager->asBinder().get(),
                                        VibratorManager::makeServiceName("default").c_str());
    CHECK_EQ(status, STATUS_OK);

    LOG(INFO) << "Kebab RichTap vibrator service started";
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
