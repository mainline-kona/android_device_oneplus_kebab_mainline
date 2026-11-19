#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/oneplus/kebab_mainline
FIRMWARE_PATH ?= device/oneplus/kebab_mainline-firmware

TARGET_BOARD_PLATFORM := qcom
TARGET_QCOM_SOC := sm8250
TARGET_GRAPHICS := mesa
TARGET_BOOT_HAL := qcom-caf-aidl

TARGET_LIGHT_HAL_SCAN_FOR_BACKLIGHT_DEVICES := true

# Suspend/resume isn't brought up yet: the USB gadget drops and the phone can hang on resume
TARGET_SUPPORTS_SUSPEND := false

# Hardware video codecs: venus (bound at post-fs, /dev/video0 dec, /dev/video1 enc)
# through the V4L2 Codec2 HAL, with kebab's own codec list
TARGET_MEDIA_C2_HAL := v4l2_codec2
TARGET_V4L2_CODEC2_USE_EXAMPLE_CONFIGURATION := false

$(call inherit-product, device/mainline/qcom-common/mainline_qcom-common.mk)

# Media (V4L2 Codec2, see TARGET_MEDIA_C2_HAL above)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/media/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.v4l2_codec2.decode_concurrent_instances=8 \
    ro.vendor.v4l2_codec2.encode_concurrent_instances=8 \
    debug.stagefright.c2-poolmask=0xf50000

# A/B
PRODUCT_PACKAGES += \
    checkpoint_gc \
    otapreopt_script \
    linux_firmware_qcom-a650

PRODUCT_SOONG_NAMESPACES += \
    hardware/qcom-caf/bootctrl

# Audio
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/audio/ucm2/OnePlus-8T.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/conf.d/sm8250/OnePlus-8T.conf \
    $(DEVICE_PATH)/audio/ucm2/HiFi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Qualcomm/sm8250/OnePlus-8T/HiFi.conf

# Bluetooth: QCA6390 has no programmed public address, so hci0 stays
# unconfigured until one is set (derived from the serial number, as on phone2)
PRODUCT_PACKAGES += \
    bdaddr

# DSP: the SLPI runs the sensor core; serve it the stock sensor configs,
# registry and socinfo through hexagonrpcd (same layout as phone2_mainline)
PRODUCT_PACKAGES += \
    hexagonrpcd_sdsp_phony

PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(DEVICE_PATH)/socinfo/,$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/socinfo/) \
    $(call find-copy-subdir-files,*,vendor/oneplus/sm8250-common/proprietary/vendor/etc/sensors/config/,$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/config/) \
    vendor/oneplus/sm8250-common/proprietary/vendor/etc/sensors/sns_reg_config:$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/sns_reg.conf

# Boot animation
TARGET_SCREEN_HEIGHT := 2400
TARGET_SCREEN_WIDTH := 1080
TARGET_BOOT_ANIMATION_RES := 1080

# Dalvik heap
$(call inherit-product, frameworks/native/build/phone-xhdpi-6144-dalvik-heap.mk)

# Dynamic partitions
PRODUCT_USE_DYNAMIC_PARTITIONS := true
$(call inherit-product, $(SRC_TARGET_DIR)/product/virtual_ab_ota.mk)

# Ramdisk fstab for first stage mount
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/system/etc/fstab.kebab \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/fstab.kebab \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/system/etc/fstab.qcom \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/fstab.qcom \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/system/etc/fstab.default \
    $(DEVICE_PATH)/init/fstab.kebab:$(TARGET_COPY_OUT_RAMDISK)/fstab.default \
    $(DEVICE_PATH)/init/init.kebab.rc:$(TARGET_COPY_OUT_RAMDISK)/init.kebab.rc \
    $(DEVICE_PATH)/init/init.kebab.rc:$(TARGET_COPY_OUT_RAMDISK)/init.qcom.rc

# Recovery helpers
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/init/firmware_mount_bind.sh:$(TARGET_COPY_OUT_RECOVERY)/root/system/bin/firmware_mount_bind.sh \
    $(DEVICE_PATH)/init/usb_gadget_bind.sh:$(TARGET_COPY_OUT_RECOVERY)/root/system/bin/usb_gadget_bind.sh \
    $(DEVICE_PATH)/init/init.recovery.qcom.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.qcom.rc \
    $(DEVICE_PATH)/init/init.recovery.qcom.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.kebab.rc \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RAMDISK)/system/etc/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RAMDISK)/system/etc/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_RAMDISK)/adb_keys \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_RECOVERY)/root/adb_keys \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_PRODUCT)/etc/security/adbd_keys

PRODUCT_SYSTEM_PROPERTIES += \
    ro.adb.secure=0

# Mainline kernels have no /dev/ashmem (removed in 5.18). libcutils only uses
# memfd for apps targeting SDK >= 37 unless forced, so older apps (SetupWizard,
# ASI, CursorWindow users) crash in Bitmap_copyAshmem without this.
PRODUCT_SYSTEM_PROPERTIES += \
    sys.use_memfd=true

PRODUCT_PRODUCT_PROPERTIES += \
    ro.adb.secure=0

PRODUCT_PROPERTY_OVERRIDES += \
    ro.adb.secure=0 \
    persist.sys.usb.config=adb \
    sys.usb.controller=a600000.usb \
    vendor.usb.controller=a600000.usb

ifneq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.usb.config=mtp,adb
endif

# Init
PRODUCT_PACKAGES += \
    bringup-log.sh \
    fstab.default \
    fstab.kebab \
    fstab.kebab.vendor_ramdisk \
    fstab.qcom \
    init.kebab.rc \
    init.qcom.rc \
    ueventd.kebab.rc

# Debug tools
$(call inherit-product-if-exists, hardware/samsung-ext/interfaces/debug-tools/debug.mk)

PRODUCT_OTA_ENFORCE_VINTF_KERNEL_REQUIREMENTS := false

# Overlays
PRODUCT_PACKAGES += \
    KebabMainlineFrameworksResTarget \
    KebabMainlineKeyHandlerResTarget \
    KebabMainlineOPlusFrameworksResTarget \
    KebabMainlineOPlusSettingsProviderResTarget \
    KebabMainlineOPlusSettingsResTarget \
    KebabMainlineOPlusSystemUIResTarget \
    KebabMainlineTelephonyResTarget \
    KebabMainlineWifiResTarget

# Radio: AT-based RIL (from phone2_mainline) on the SDX55's wwan0at0,
# with kebab-dataconn bringing up data over QMI (wwan0qmi0 + mhi_hwip0)
PRODUCT_PACKAGES += \
    kebab-dataconn \
    libcuttlefish-ril-2 \
    libcuttlefish-rild

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.calling.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.calling.xml \
    frameworks/native/data/etc/android.hardware.telephony.data.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.data.xml \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.messaging.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.messaging.xml \
    frameworks/native/data/etc/android.hardware.telephony.radio.access.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.radio.access.xml \
    frameworks/native/data/etc/android.hardware.telephony.subscription.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.subscription.xml

# Scoped storage
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH) \
    hardware/mainline/common \
    hardware/mainline/kona-ril
