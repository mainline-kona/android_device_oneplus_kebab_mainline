/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * CameraProvider@2.4 legacy/0 HAL service for OnePlus 8T (kebab, SM8250).
 * Compiles camhidl_shim.cpp directly into the main executable with
 * -Wl,--export-dynamic so IMapper@3.0 / IQtiMapper@3.0 / IPostProcService@1.0
 * interposition works under SELinux Enforcing (where AT_SECURE strips LD_PRELOAD).
 */

#define LOG_TAG "android.hardware.camera.provider@2.4-service_64.kebab"

#include <android/hardware/camera/provider/2.4/ICameraProvider.h>
#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <hidl/LegacySupport.h>
#include <log/log.h>
#include <malloc.h>
#include <string.h>

using android::status_t;
using android::hardware::defaultPassthroughServiceImplementation;
using android::hardware::camera::provider::V2_4::ICameraProvider;

int main(int argc, char** argv) {
    /*
     * SenseTime libcvface_api.so validates /proc/self/cmdline against the stock
     * binary path (/vendor/bin/hw/android.hardware.camera.provider@2.4-service_64).
     * Strip our ".kebab" suffix in-place so /proc/self/cmdline matches stock while
     * avoiding Soong install/symbol collisions with AOSP's provider binary.
     */
    if (argc > 0 && argv[0] != nullptr) {
        static constexpr char kSuffix[] = ".kebab";
        size_t len = strlen(argv[0]);
        size_t slen = sizeof(kSuffix) - 1;
        if (len > slen && strcmp(argv[0] + len - slen, kSuffix) == 0) {
            memset(argv[0] + len - slen, 0, slen);
        }
    }

    ALOGI("CameraProvider@2.4 legacy service (kebab in-process shim) is starting.");
    android::ProcessState::initWithDriver("/dev/vndbinder");

    if (property_get_bool("ro.vendor.camera.provider24.disable_mem_init", false)) {
        if (mallopt(M_BIONIC_ZERO_INIT, 0) == 0) {
            ALOGE("Disabling heap initialization failed.");
        }
    }

    status_t status =
            defaultPassthroughServiceImplementation<ICameraProvider>("legacy/0", /*maxThreads*/ 6);
    return status;
}
