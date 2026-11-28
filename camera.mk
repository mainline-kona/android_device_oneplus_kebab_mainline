#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#
# Stock OnePlus 8T (kebab, SM8250) CamX / CHI camera stack over mainline ACK 6.18
# + out-of-tree kona-camera-kernel (camera.ko + synx) + QTI sm8850 Gralloc.
#

# Stock 2020-era camera blobs are 4K-aligned
PRODUCT_CHECK_PREBUILT_MAX_PAGE_SIZE := false

# Camera provider HAL (HIDL 2.4 legacy/0) + CPU OpenCL + HIDL mapper/postproc shim
PRODUCT_PACKAGES += \
    android.hardware.camera.provider@2.4-impl \
    android.hardware.camera.provider@2.4-service_64 \
    libOpenCL_cpu \
    libcamhidl_shim

# Vendor compat libraries required by stock CamX / CHI / feature2 / ArcSoft blobs
PRODUCT_PACKAGES += \
    android.hardware.graphics.mapper@2.0.vendor \
    android.hardware.graphics.mapper@2.1.vendor \
    android.hardware.graphics.mapper@3.0.vendor \
    libcamera_metadata_shim \
    libhidltransport.vendor \
    libhwbinder.vendor \
    libprotobuf-cpp-full-3.9.1-vendorcompat \
    libprotobuf-cpp-lite-3.9.1-vendorcompat \
    vendor.display.config@1.0.vendor \
    vendor.display.config@1.1.vendor \
    vendor.display.config@1.2.vendor \
    vendor.display.config@1.3.vendor \
    vendor.display.config@1.4.vendor \
    vendor.display.config@1.5.vendor \
    vendor.display.config@1.6.vendor \
    vendor.display.config@1.7.vendor \
    vendor.display.config@1.8.vendor \
    vendor.display.config@1.9.vendor \
    vendor.display.config@2.0.vendor \
    vendor.qti.hardware.camera.postproc@1.0.vendor \
    vendor.qti.hardware.display.mapper@1.0.vendor \
    vendor.qti.hardware.display.mapper@2.0.vendor \
    vendor.qti.hardware.display.mapper@3.0.vendor \
    vendor.qti.hardware.display.mapperextensions@1.0.vendor \
    vendor.qti.hardware.display.mapperextensions@1.1.vendor

# CamX runtime override config & camera feature permissions
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/configs/camera/camxoverridesettings.txt:$(TARGET_COPY_OUT_VENDOR)/etc/camera/camxoverridesettings.txt \
    frameworks/native/data/etc/android.hardware.camera.flash-autofocus.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.flash-autofocus.xml \
    frameworks/native/data/etc/android.hardware.camera.front.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.front.xml \
    frameworks/native/data/etc/android.hardware.camera.full.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.full.xml \
    frameworks/native/data/etc/android.hardware.camera.raw.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.camera.raw.xml

# Stock OnePlus 8T camera firmware, HAL, CHI, tuning, sensor & algo blobs
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(FIRMWARE_PATH)/camera/vendor/,$(TARGET_COPY_OUT_VENDOR)/) \
    $(call find-copy-subdir-files,*,$(FIRMWARE_PATH)/camera/odm/,$(TARGET_COPY_OUT_ODM)/)
