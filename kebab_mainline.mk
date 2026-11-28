#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
TARGET_SUPPORTS_OMX_SERVICE := false
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base_telephony.mk)

# Inherit from kebab mainline device
$(call inherit-product, device/oneplus/kebab_mainline/device.mk)

# Prevent device/qcom/common/common.mk from adding nonexistent hardware/qcom-caf/qcom
QCOM_SOONG_NAMESPACE := hardware/qcom-caf/sm8850

# Inherit from the Neoteric configuration
$(call inherit-product, vendor/neoteric/target/product/neoteric-target.mk)

# Clear downstream QTI display flags set by device/qcom/common/common.mk
# that do not exist on mainline (no smmu_proxy_uapi_header or UBWCP).
SOONG_CONFIG_qtidisplay_drmpp := false
SOONG_CONFIG_qtidisplay_smmu_proxy := false
SOONG_CONFIG_qtidisplay_ubwcp_headers := false

# Exclude unused CLO downstream NXP HAL repos that depend on proprietary
# QTI peripheralstate_headers / mink_headers (we use AOSP hardware/nxp/nfc).
PRODUCT_SOURCE_ROOT_DIRS += \
    -vendor/nxp/opensource/halimpl \
    -vendor/nxp/opensource/libnfc_halimpl_extn

PRODUCT_NAME := kebab_mainline
PRODUCT_DEVICE := kebab_mainline
PRODUCT_MANUFACTURER := OnePlus
PRODUCT_BRAND := OnePlus
PRODUCT_MODEL := KB2005

PRODUCT_GMS_CLIENTID_BASE := android-oneplus

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc="OnePlus8T-user 14 UKQ1.230924.001 R.1a46c89-1_1 release-keys" \
    BuildFingerprint=OnePlus/OnePlus8T/OnePlus8T:14/UKQ1.230924.001/R.1a46c89-1_1:user/release-keys \
    DeviceName=OnePlus8T \
    DeviceProduct=OnePlus8T \
    SystemDevice=OnePlus8T \
    SystemName=OnePlus8T
