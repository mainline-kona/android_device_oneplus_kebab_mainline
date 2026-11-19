#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

DEVICE_PATH := device/oneplus/kebab_mainline
KERNEL_PATH := $(DEVICE_PATH)/prebuilt

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
BOARD_PREBUILT_DTBIMAGE_DIR := $(DEVICE_PATH)/prebuilt/dtb
BOARD_PREBUILT_DTBOIMAGE := $(DEVICE_PATH)/prebuilt/dtbo.img
BOARD_DTBOIMG_PARTITION_SIZE := 25165824

# Kernel
TARGET_PREBUILT_KERNEL := $(DEVICE_PATH)/prebuilt/Image
TARGET_KERNEL_MIXED_MODE := false
