/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Fingerprint.h"
#include "FingerprintConfig.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

using ::aidl::android::hardware::biometrics::fingerprint::Fingerprint;
using ::aidl::android::hardware::biometrics::fingerprint::FingerprintConfig;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);

    std::shared_ptr<FingerprintConfig> config = std::make_shared<FingerprintConfig>();
    config->init();

    std::shared_ptr<Fingerprint> fingerprint = ndk::SharedRefBase::make<Fingerprint>(config);
    if (!fingerprint->isInitialized()) {
        LOG(ERROR) << "Fingerprint HAL failed to initialize; not registering AIDL service";
        return EXIT_FAILURE;
    }

    const std::string instance = std::string() + Fingerprint::descriptor + "/default";
    binder_status_t status =
            AServiceManager_addService(fingerprint->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK);

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;  // should not reach
}
