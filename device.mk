#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/oneplus/kebab_mainline

TARGET_BOARD_PLATFORM := qcom
TARGET_QCOM_SOC := sm8250
TARGET_GRAPHICS := mesa
TARGET_BOOT_HAL := qcom-caf-aidl

TARGET_LIGHT_HAL_SCAN_FOR_BACKLIGHT_DEVICES := true

# Suspend/resume isn't brought up yet: the USB gadget drops and the phone can hang on resume
TARGET_SUPPORTS_SUSPEND := false

$(call inherit-product, device/mainline/qcom-common/mainline_qcom-common.mk)

# A/B
PRODUCT_PACKAGES += \
    checkpoint_gc \
    otapreopt_script

PRODUCT_SOONG_NAMESPACES += \
    hardware/qcom-caf/bootctrl

# Audio
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/audio/ucm2/OnePlus-8T.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/conf.d/sm8250/OnePlus-8T.conf \
    $(DEVICE_PATH)/audio/ucm2/HiFi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Qualcomm/sm8250/OnePlus-8T/HiFi.conf

# Boot animation
TARGET_SCREEN_HEIGHT := 2400
TARGET_SCREEN_WIDTH := 1080
TARGET_BOOT_ANIMATION_RES := 1080

# Dalvik heap
$(call inherit-product, frameworks/native/build/phone-xhdpi-6144-dalvik-heap.mk)

# Dynamic partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true

# Init
PRODUCT_PACKAGES += \
    bringup-log.sh \
    fstab.kebab \
    fstab.kebab.vendor_ramdisk \
    init.kebab.rc \
    ueventd.kebab.rc

PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

# Overlays
PRODUCT_PACKAGES += \
    KebabMainlineFrameworksResTarget \
    KebabMainlineKeyHandlerResTarget \
    KebabMainlineOPlusFrameworksResTarget \
    KebabMainlineOPlusSettingsProviderResTarget \
    KebabMainlineOPlusSettingsResTarget \
    KebabMainlineOPlusSystemUIResTarget

# Scoped storage
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH) \
    hardware/mainline/common
