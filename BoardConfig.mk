#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/oneplus/kebab_mainline
TARGET_KERNEL_DIR ?= device/oneplus/kebab_mainline-kernel

TARGET_INITIAL_BRINGUP := true
TARGET_BOARD_PLATFORM := qcom
TARGET_QCOM_SOC := sm8250
TARGET_QCOM_SOC_FAMILY := sm8250
TARGET_GRAPHICS := mesa
TARGET_GRAPHICS_EGL := mesa
TARGET_GRAPHICS_VULKAN := mesa
BOARD_MESA3D_USES_MESON_BUILD := true

include device/mainline/qcom-common/BoardConfigMainlineQcomCommon.mk

# AVB
BOARD_AVB_ENABLE := true
BOARD_AVB_MAKE_VBMETA_IMAGE_ARGS += --flags 3

# Build
BUILD_BROKEN_ELF_PREBUILT_PRODUCT_COPY_FILES := true

# Boot
BOARD_BOOT_HEADER_VERSION := 2
BOARD_MKBOOTIMG_ARGS := --header_version $(BOARD_BOOT_HEADER_VERSION)
BOARD_KERNEL_BASE := 0x00000000
BOARD_KERNEL_PAGESIZE := 4096
BOARD_RAMDISK_USE_LZ4 := true
BOARD_KERNEL_CMDLINE := \
    $(filter-out binder.impl=rust panic=-1 vt.global_cursor_default=0,$(MAINLINE_COMMON_KERNEL_PARAMS)) \
    $(MAINLINE_COMMON_ANDROIDBOOT_PARAMS) \
    $(MAINLINE_QCOM_SOC_ANDROIDBOOT_PARAMS) \
    androidboot.hardware=kebab \
    clk_ignore_unused pd_ignore_unused regulator_ignore_unused \
    msm.separate_gpu_kms=1 \
    console=tty0 ignore_loglevel panic=0

# DTB / DTBO
BOARD_INCLUDE_DTB_IN_BOOTIMG := true
BOARD_PREBUILT_DTBIMAGE_DIR := $(TARGET_KERNEL_DIR)/dtb
BOARD_PREBUILT_DTBOIMAGE := $(TARGET_KERNEL_DIR)/dtbo.img
BOARD_DTBOIMG_PARTITION_SIZE := 25165824

# Kernel
TARGET_PREBUILT_KERNEL := $(TARGET_KERNEL_DIR)/Image
TARGET_KERNEL_MIXED_MODE := false

# Partitions - A/B
AB_OTA_UPDATER := true
AB_OTA_PARTITIONS += \
    boot \
    dtbo \
    odm \
    product \
    recovery \
    system \
    system_ext \
    vbmeta \
    vbmeta_system \
    vendor

# Partitions - Dynamic
BOARD_SUPER_PARTITION_GROUPS := oneplus_dynamic_partitions
BOARD_ONEPLUS_DYNAMIC_PARTITIONS_PARTITION_LIST := odm product system system_ext vendor
BOARD_ONEPLUS_DYNAMIC_PARTITIONS_SIZE := 7511998464
BOARD_SUPER_PARTITION_SIZE := 7516192768

# Partitions - Filesystems
BOARD_ODMIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_PRODUCTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_SYSTEM_EXTIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_VENDORIMAGE_FILE_SYSTEM_TYPE := erofs
BOARD_USES_METADATA_PARTITION := true
TARGET_USERIMAGES_USE_EXT4 := true
TARGET_USERIMAGES_USE_F2FS := true

# Partitions - Paths
TARGET_COPY_OUT_ODM := odm
TARGET_COPY_OUT_PRODUCT := product
TARGET_COPY_OUT_SYSTEM_EXT := system_ext
TARGET_COPY_OUT_VENDOR := vendor

# Partitions - Sizes
BOARD_BOOTIMAGE_PARTITION_SIZE := 100663296
BOARD_RECOVERYIMAGE_PARTITION_SIZE := 100663296
BOARD_USERDATAIMAGE_PARTITION_SIZE := 236009631744

# Recovery
TARGET_RECOVERY_FSTAB := $(DEVICE_PATH)/init/fstab.kebab
TARGET_RECOVERY_PIXEL_FORMAT := RGBX_8888
TARGET_SCREEN_DENSITY := 450

# Properties
TARGET_VENDOR_PROP += $(DEVICE_PATH)/vendor.prop

# SELinux
BOARD_VENDOR_SEPOLICY_DIRS += $(DEVICE_PATH)/sepolicy/vendor
