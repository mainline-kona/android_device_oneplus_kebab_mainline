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
