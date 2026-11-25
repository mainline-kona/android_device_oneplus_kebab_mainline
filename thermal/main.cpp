//
// Copyright (C) 2026 The PenguinOS Project
// SPDX-License-Identifier: Apache-2.0
//

#define LOG_TAG "ThermalHalKona"

#include "Thermal.h"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <cstdlib>
#include <string>

using aidl::android::hardware::thermal::impl::kona::Thermal;

int main(int /*argc*/, char** argv) {
    ::android::base::InitLogging(argv);
    ABinderProcess_setThreadPoolMaxThreadCount(2);

    std::shared_ptr<Thermal> thermal = ndk::SharedRefBase::make<Thermal>();
    const std::string instance = std::string(Thermal::descriptor) + "/default";
    binder_status_t status =
            AServiceManager_addService(thermal->asBinder().get(), instance.c_str());
    CHECK(status == STATUS_OK) << "Failed to register " << instance << ": " << status;

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
