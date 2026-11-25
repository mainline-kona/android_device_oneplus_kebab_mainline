#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/oneplus/kebab_mainline
FIRMWARE_PATH ?= device/oneplus/kebab_mainline-firmware

# Sensors: real hardware via the mainline Sensors HAL + libssc (SSC/QMI)
# backend. Must be set before the qcom-common inherit below activates the
# optional sensors-hal_mainline module.
TARGET_SENSORS_HAL := mainline

# Vibrator: AAC RichTap X-axis LRA AIDL V4 HAL over AW8697 /dev/awinic_haptic
# (replaces optional vibrator-hal_mainline APEX).
TARGET_VIBRATOR_HAL := richtap

TARGET_BOARD_PLATFORM := qcom
TARGET_QCOM_SOC := sm8250
TARGET_GRAPHICS := mesa
TARGET_BOOT_HAL := qcom-caf-aidl

TARGET_LIGHT_HAL_SCAN_FOR_BACKLIGHT_DEVICES := true

# Suspend/resume isn't brought up yet: the USB gadget drops and the phone can hang on resume
TARGET_SUPPORTS_SUSPEND := false

# Hardware video codecs: venus (/dev/video0 enc, /dev/video1 dec)
# through the V4L2 Codec2 HAL, with kebab's own codec list
TARGET_MEDIA_C2_HAL := v4l2_codec2
TARGET_V4L2_CODEC2_USE_EXAMPLE_CONFIGURATION := false

# Hardware-backed security: stock Qualcomm Keymaster 4.1 + Gatekeeper 1.0 + qseecomd
# over our open-source external/qcomtee (libQSEEComAPI) + kernel qcom_qseecom_bridge.
# Keymaster 4.1 and Gatekeeper 1.0 are HIDL HALs, so keep hwservicemanager enabled
# on Android 17 (installs hwservicemanager_no_max.xml in /system_ext/etc/vintf).
TARGET_SUPPORTS_HARDWARE_BACKED_SECURITY := true
PRODUCT_HIDL_ENABLED := true

$(call inherit-product, device/mainline/qcom-common/mainline_qcom-common.mk)

# Media (V4L2 Codec2, see TARGET_MEDIA_C2_HAL above)
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/media/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.v4l2_codec2.decoder.supported.h264=true \
    ro.vendor.v4l2_codec2.decoder.supported.hevc=true \
    ro.vendor.v4l2_codec2.decoder.supported.vp8=true \
    ro.vendor.v4l2_codec2.decoder.supported.vp9=true \
    ro.vendor.v4l2_codec2.encoder.supported.h264=true \
    ro.vendor.v4l2_codec2.decode_concurrent_instances=8 \
    ro.vendor.v4l2_codec2.encode_concurrent_instances=8 \
    debug.stagefright.c2-poolmask=0x1f50000

# A/B
PRODUCT_PACKAGES += \
    checkpoint_gc \
    otapreopt_script \
    linux_firmware_qcom-a650

# Graphics (Mesa Turnip Vulkan 1.3 on Adreno 650)
# Install 00-turnip-defaults.conf as /vendor/etc/drirc (vendor_configs_file,
# readable by all domains under Enforcing SELinux) so tu_emulate_second_queue
# activates for Android HWUI ("android framework").
PRODUCT_COPY_FILES += \
    external/mesa/src/freedreno/vulkan/00-turnip-defaults.conf:$(TARGET_COPY_OUT_VENDOR)/etc/drirc \
    frameworks/native/data/etc/android.hardware.vulkan.compute-0.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.compute.xml \
    frameworks/native/data/etc/android.hardware.vulkan.level-1.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.level.xml \
    frameworks/native/data/etc/android.hardware.vulkan.version-1_3.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.vulkan.version.xml \
    frameworks/native/data/etc/android.software.opengles.deqp.level-2026-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.opengles.deqp.level.xml \
    frameworks/native/data/etc/android.software.vulkan.deqp.level-2026-03-01.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.software.vulkan.deqp.level.xml

PRODUCT_SOONG_NAMESPACES += \
    hardware/qcom-caf/bootctrl

# Audio
PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/audio/ucm2/OnePlus-8T.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/conf.d/sm8250/OnePlus-8T.conf \
    $(DEVICE_PATH)/audio/ucm2/HiFi.conf:$(TARGET_COPY_OUT_VENDOR)/etc/alsa/ucm2/Qualcomm/sm8250/OnePlus-8T/HiFi.conf

# GNSS: QMI LOC over QRTR + AIDL HAL
PRODUCT_PACKAGES += \
    android.hardware.gnss-service.kona \
    kona-gnss

# Thermal: AIDL v3 HAL backed by mainline /sys/class/thermal
PRODUCT_PACKAGES += \
    android.hardware.thermal-service.kona

# Health: AIDL v5 HAL (2S bq28z610 normalization, SOH, MP2650 policy, UFS health)
PRODUCT_PACKAGES += \
    android.hardware.health-service.kona \
    android.hardware.health-service.kona_recovery

# Radio: AIDL v5 RIL (libcuttlefish-rild) + QMI multi-APN data (kona-dataconn)
# + Open AOSP VoLTE / IMS stack (generic-ims: ImsStack, ImsMedia, Iwlan, QNS, CarrierSettings)
$(call inherit-product, hardware/lineage/generic-ims/ims.mk)

PRODUCT_PACKAGES += \
    kona-dataconn \
    libcuttlefish-rild

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.calling.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.calling.xml \
    frameworks/native/data/etc/android.hardware.telephony.data.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.data.xml \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.messaging.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.messaging.xml \
    frameworks/native/data/etc/android.hardware.telephony.radio.access.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.radio.access.xml \
    frameworks/native/data/etc/android.hardware.telephony.subscription.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.subscription.xml

PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/configs/gps.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gps.conf \
    $(DEVICE_PATH)/configs/gps_debug.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gps_debug.conf \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml

# NFC
PRODUCT_PACKAGES += \
    android.hardware.nfc2-service.nxp \
    NfcNci \
    Tag

PRODUCT_COPY_FILES += \
    $(DEVICE_PATH)/nfc/init.kebab-mainline.nfc.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.kebab-mainline.nfc.rc \
    $(DEVICE_PATH)/nfc/libnfc-nci.conf:$(TARGET_COPY_OUT_VENDOR)/etc/libnfc-nci.conf \
    $(DEVICE_PATH)/nfc/libnfc-nxp.conf:$(TARGET_COPY_OUT_VENDOR)/etc/libnfc-nxp.conf \
    frameworks/native/data/etc/com.nxp.mifare.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/com.nxp.mifare.xml \
    frameworks/native/data/etc/android.hardware.nfc.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.xml \
    frameworks/native/data/etc/android.hardware.nfc.hce.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hce.xml \
    frameworks/native/data/etc/android.hardware.nfc.hcef.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.nfc.hcef.xml

# Bluetooth: QCA6390 has no programmed public address, so hci0 stays
# unconfigured until one is set (derived from the serial number, as on phone2)
PRODUCT_PACKAGES += \
    bdaddr

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml

# Bluetooth system_ext properties (persist.bluetooth.* is bluetooth_prop,
# not writable by vendor_init)
PRODUCT_SYSTEM_EXT_PROPERTIES += \
    persist.bluetooth.opus.enabled=true \
    persist.bluetooth.bqr.choppy_threshold=9,6 \
    persist.bluetooth.bqr.event_mask=0x8000000E \
    persist.bluetooth.bqr.min_interval_ms=60000

# QTEE / QSEECom (open-source external/qcomtee + kernel qcom_qseecom_bridge)
# + stock qseecomd RPMB/storage listeners + hardware Keymaster 4.1 & Gatekeeper 1.0
PRODUCT_PACKAGES += \
    hwservicemanager \
    libQSEEComAPI \
    libion_dmaheap \
    libxml2.vendor \
    android.hardware.gatekeeper@1.0.vendor \
    android.hardware.keymaster@4.0.vendor \
    android.hardware.keymaster@4.1.vendor

PRODUCT_COPY_FILES += \
    $(FIRMWARE_PATH)/qtee/bin/qseecomd:$(TARGET_COPY_OUT_VENDOR)/bin/qseecomd \
    $(FIRMWARE_PATH)/qtee/etc/gpfspath_oem_config.xml:$(TARGET_COPY_OUT_VENDOR)/etc/gpfspath_oem_config.xml \
    $(FIRMWARE_PATH)/qtee/bin/hw/android.hardware.gatekeeper@1.0-service-qti:$(TARGET_COPY_OUT_VENDOR)/bin/hw/android.hardware.gatekeeper@1.0-service-qti \
    $(FIRMWARE_PATH)/qtee/etc/init/android.hardware.gatekeeper@1.0-service-qti.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.gatekeeper@1.0-service-qti.rc \
    $(FIRMWARE_PATH)/qtee/lib64/hw/android.hardware.gatekeeper@1.0-impl-qti.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/android.hardware.gatekeeper@1.0-impl-qti.so \
    $(FIRMWARE_PATH)/qtee/bin/hw/android.hardware.keymaster@4.1-service-qti:$(TARGET_COPY_OUT_VENDOR)/bin/hw/android.hardware.keymaster@4.1-service-qti \
    $(FIRMWARE_PATH)/qtee/etc/init/android.hardware.keymaster@4.1-service-qti.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/android.hardware.keymaster@4.1-service-qti.rc \
    $(FIRMWARE_PATH)/qtee/lib64/librpmb.so:$(TARGET_COPY_OUT_VENDOR)/lib64/librpmb.so \
    $(FIRMWARE_PATH)/qtee/lib64/libssd.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libssd.so \
    $(FIRMWARE_PATH)/qtee/lib64/libdrmfs.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libdrmfs.so \
    $(FIRMWARE_PATH)/qtee/lib64/libdrmtime.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libdrmtime.so \
    $(FIRMWARE_PATH)/qtee/lib64/libqisl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libqisl.so \
    $(FIRMWARE_PATH)/qtee/lib64/libspl.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libspl.so \
    $(FIRMWARE_PATH)/qtee/lib64/libGPreqcancel.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libGPreqcancel.so \
    $(FIRMWARE_PATH)/qtee/lib64/libGPreqcancel_svc.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libGPreqcancel_svc.so \
    $(FIRMWARE_PATH)/qtee/lib64/libdiag.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libdiag.so \
    $(FIRMWARE_PATH)/qtee/lib64/libtime_genoff.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libtime_genoff.so \
    $(FIRMWARE_PATH)/qtee/lib64/libkeymasterdeviceutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libkeymasterdeviceutils.so \
    $(FIRMWARE_PATH)/qtee/lib64/libkeymasterutils.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libkeymasterutils.so \
    $(FIRMWARE_PATH)/qtee/lib64/libqtikeymaster4.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libqtikeymaster4.so \
    $(FIRMWARE_PATH)/qtee/lib64/libqcbor.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libqcbor.so \
    $(FIRMWARE_PATH)/qtee/lib64/libspcom.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libspcom.so

# Vibrator: AAC RichTap X-axis LRA AIDL V4 HAL (AW8697 /dev/awinic_haptic)
PRODUCT_PACKAGES += \
    android.hardware.vibrator-service.kebab

# Wi-Fi: AIDL V4 IWifi HAL over mainline ath11k_pci (pure nl80211 libwifi-hal-desktop)
PRODUCT_PACKAGES += \
    android.hardware.wifi-service

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.wifi.direct.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.direct.xml \
    frameworks/native/data/etc/android.hardware.wifi.passpoint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.passpoint.xml

PRODUCT_COPY_FILES += \
    $(FIRMWARE_PATH)/richtap/lib64/libaacvibrator.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libaacvibrator.so \
    $(FIRMWARE_PATH)/richtap/etc/aac_richtap.config:$(TARGET_COPY_OUT_VENDOR)/etc/aac_richtap.config

# Fingerprint: Goodix optical UDFPS AIDL HAL (G6_3 gfp9638 & G6_7 gfp9678) over QTEE
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.kebab

PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml \
    $(FIRMWARE_PATH)/fingerprint/lib64/hw/fingerprint.19805_goodix9638_G6_3.default.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/fingerprint.19805_goodix9638_G6_3.default.so \
    $(FIRMWARE_PATH)/fingerprint/lib64/libgf_hal_19805_G6_3_oplus.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libgf_hal_19805_G6_3_oplus.so \
    $(FIRMWARE_PATH)/fingerprint/lib64/hw/fingerprint.19805_goodix9678_G6_7.default.so:$(TARGET_COPY_OUT_VENDOR)/lib64/hw/fingerprint.19805_goodix9678_G6_7.default.so \
    $(FIRMWARE_PATH)/fingerprint/lib64/libgf_hal_19805_G6_7_oplus.so:$(TARGET_COPY_OUT_VENDOR)/lib64/libgf_hal_19805_G6_7_oplus.so \
    $(call find-copy-subdir-files,*,$(FIRMWARE_PATH)/fingerprint/firmware/,$(TARGET_COPY_OUT_VENDOR)/firmware/)

# DSP: the SLPI runs the sensor core; serve it the stock sensor configs,
# registry and socinfo through hexagonrpcd (same layout as phone2_mainline)
PRODUCT_PACKAGES += \
    hexagonrpcd_sdsp_phony \
    pd-mapper

PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(DEVICE_PATH)/socinfo/,$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/socinfo/) \
    $(call find-copy-subdir-files,*,$(DEVICE_PATH)/sensors/config/,$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/config/) \
    $(DEVICE_PATH)/sensors/sns_reg.conf:$(TARGET_COPY_OUT_VENDOR)/etc/hexagonrpcd-root/sensors/sns_reg.conf

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
    $(DEVICE_PATH)/init/init.recovery.qcom.rc:$(TARGET_COPY_OUT_RECOVERY)/root/init.recovery.kebab.rc

# Firmware
PRODUCT_COPY_FILES += \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/amss.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/QCA6390/hw2.0/amss.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/board-2.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/QCA6390/hw2.0/board-2.bin \
    $(FIRMWARE_PATH)/ath11k/QCA6390/hw2.0/m3.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/ath11k/QCA6390/hw2.0/m3.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_VENDOR)/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/qca/htbtfw20.tlv:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qca/htbtfw20.tlv \
    $(FIRMWARE_PATH)/qca/htnv20.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qca/htnv20.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RECOVERY)/root/system/etc/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/a650_sqe.fw:$(TARGET_COPY_OUT_RAMDISK)/system/etc/firmware/qcom/a650_sqe.fw \
    $(FIRMWARE_PATH)/qcom/a650_gmu.bin:$(TARGET_COPY_OUT_RAMDISK)/system/etc/firmware/qcom/a650_gmu.bin \
    $(FIRMWARE_PATH)/qcom/sm8250/OnePlus/kebab/a650_zap.mbn:$(TARGET_COPY_OUT_VENDOR)/firmware/qcom/sm8250/OnePlus/kebab/a650_zap.mbn \
    $(FIRMWARE_PATH)/qcom/sm8250/OnePlus/kebab/a650_zap.mbn:$(TARGET_COPY_OUT_RAMDISK)/lib/firmware/qcom/sm8250/OnePlus/kebab/a650_zap.mbn \
    $(FIRMWARE_PATH)/qcom/sm8250/OnePlus/kebab/a650_zap.mbn:$(TARGET_COPY_OUT_RECOVERY)/root/lib/firmware/qcom/sm8250/OnePlus/kebab/a650_zap.mbn \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_RAMDISK)/adb_keys \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_RECOVERY)/root/adb_keys \
    $(FIRMWARE_PATH)/adb_keys:$(TARGET_COPY_OUT_PRODUCT)/etc/security/adbd_keys

PRODUCT_SYSTEM_PROPERTIES += \
    ro.adb.secure=0

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
    kebab-pdready.sh \
    pd-mapper.rc \
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

# SELinux denial collector (userdebug/eng)
TARGET_ENABLE_SELOGGER := true

# Overlays
PRODUCT_PACKAGES += \
    KebabMainlineFrameworksResTarget \
    KebabMainlineKeyHandlerResTarget \
    KebabMainlineOPlusFrameworksResTarget \
    KebabMainlineOPlusSettingsProviderResTarget \
    KebabMainlineOPlusSettingsResTarget \
    KebabMainlineOPlusSystemUIResTarget \
    KebabMainlineWifiResTarget

# Scoped storage
$(call inherit-product, $(SRC_TARGET_DIR)/product/emulated_storage.mk)

# Shipping API level
TARGET_FOLLOWS_LATEST_SHIPPING_API_LEVEL := true

# Soong namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(DEVICE_PATH) \
    hardware/mainline/common \
    external/mainline-hw-deps
