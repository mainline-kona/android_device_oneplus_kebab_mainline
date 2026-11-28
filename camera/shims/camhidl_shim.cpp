/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 *
 * LD_PRELOAD shim for stock OnePlus 8T (SM8250) CamX / CHI running under
 * android.hardware.camera.provider@2.4-service_64 on Android 17 with
 * QTI IMapper 5 (mapper.qti) and no HIDL mapper@2.0/3.0 or postproc@1.0
 * VINTF registrations:
 *
 *  1. Intercepts IMapper@3.0::getService() and IQtiMapper@3.0::castFrom()
 *     (used by libcamximageformatutils.so and postproc@1.0-service-impl.so)
 *     and returns an in-process IQtiMapper@3.0 + IQtiMapperExtensions@1.1
 *     backed directly by sm8850 libgralloccore (BufferManager) and
 *     libgrallocutils (GetImplDefinedFormat / GetBufferSizeAndDimensions /
 *     GetYUVPlaneInfo / GetRGBPlaneInfo).
 *  2. Intercepts IMapper@2.0::getService() to return nullptr so callers take
 *     the IMapper@3.0 path cleanly.
 *  3. Intercepts IPostProcService@1.0::registerAsService() / getService() /
 *     tryGetService() so CamX's in-process offline postproc service succeeds
 *     without declaring a deprecated HIDL HAL in VINTF.
 */

#include <QtiGralloc.h>
#include <QtiGrallocMetadata.h>
#include <android-base/logging.h>
#include <android/hardware/graphics/mapper/2.0/IMapper.h>
#include <android/hardware/graphics/mapper/3.0/IMapper.h>
#include <cutils/native_handle.h>
#include <sync/sync.h>
#include <vendor/qti/hardware/camera/postproc/1.0/IPostProcService.h>
#include <vendor/qti/hardware/display/mapper/3.0/IQtiMapper.h>
#include <vendor/qti/hardware/display/mapperextensions/1.1/IQtiMapperExtensions.h>

#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>

#include "gr_buf_descriptor.h"
#include "gr_buf_mgr.h"
#include "gr_utils.h"

using android::sp;
using android::status_t;
using android::hardware::hidl_handle;
using android::hardware::hidl_vec;
using android::hardware::Return;
using android::hardware::Void;

namespace mapper2 = android::hardware::graphics::mapper::V2_0;
namespace mapper3 = android::hardware::graphics::mapper::V3_0;
namespace qmapper3 = vendor::qti::hardware::display::mapper::V3_0;
namespace qext10 = vendor::qti::hardware::display::mapperextensions::V1_0;
namespace qext11 = vendor::qti::hardware::display::mapperextensions::V1_1;
namespace postproc10 = vendor::qti::hardware::camera::postproc::V1_0;

namespace {

using gralloc::BufferInfo;
using gralloc::BufferManager;

static mapper3::Error toMapper3Error(gralloc::Error err) {
    switch (err) {
        case gralloc::Error::NONE:
            return mapper3::Error::NONE;
        case gralloc::Error::BAD_DESCRIPTOR:
            return mapper3::Error::BAD_DESCRIPTOR;
        case gralloc::Error::BAD_BUFFER:
            return mapper3::Error::BAD_BUFFER;
        case gralloc::Error::BAD_VALUE:
            return mapper3::Error::BAD_VALUE;
        case gralloc::Error::NO_RESOURCES:
            return mapper3::Error::NO_RESOURCES;
        case gralloc::Error::UNSUPPORTED:
        default:
            return mapper3::Error::UNSUPPORTED;
    }
}

class ShimQtiMapperExtensions : public qext11::IQtiMapperExtensions {
  public:
    ShimQtiMapperExtensions() : buf_mgr_(BufferManager::GetInstance()) {}

    Return<void> getMapSecureBufferFlag(void* buffer,
                                        getMapSecureBufferFlag_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        int map_secure = 0;
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            err = static_cast<qext10::Error>(
                    gralloc::GetMetaDataValue(hnd, QTI_MAP_SECURE_BUFFER, &map_secure));
        }
        if (err != qext10::Error::NONE) {
            map_secure = 0;
        }
        hidl_cb(err, map_secure != 0);
        return Void();
    }

    Return<void> getInterlacedFlag(void* buffer, getInterlacedFlag_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        int interlaced = 0;
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            err = qext10::Error::NONE;
            auto ret = static_cast<qext10::Error>(
                    gralloc::GetMetaDataValue(hnd, QTI_PP_PARAM_INTERLACED, &interlaced));
            if (ret != qext10::Error::NONE) {
                interlaced = 0;
            }
        }
        hidl_cb(err, interlaced != 0);
        return Void();
    }

    Return<void> getCustomDimensions(void* buffer, getCustomDimensions_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        int stride = 0;
        int height = 0;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            stride = hnd->width;
            height = hnd->height;
            if (gralloc::GetCustomDimensions(hnd, &stride, &height) == 0) {
                err = qext10::Error::NONE;
            }
        }
        hidl_cb(err, stride, height);
        return Void();
    }

    Return<void> getRgbDataAddress(void* buffer, getRgbDataAddress_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        void* rgb_data = nullptr;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            if (gralloc::GetRgbDataAddress(hnd, &rgb_data) == 0) {
                err = qext10::Error::NONE;
            }
        }
        hidl_cb(err, rgb_data);
        return Void();
    }

    Return<void> calculateBufferAttributes(int32_t width, int32_t height, int32_t format,
                                           uint64_t usage,
                                           calculateBufferAttributes_cb hidl_cb) override {
        auto err = qext10::Error::NONE;
        unsigned int alignedw = 0;
        unsigned int alignedh = 0;
        BufferInfo info(width, height, format, usage);
        if (gralloc::GetAlignedWidthAndHeight(info, &alignedw, &alignedh) != 0) {
            err = qext10::Error::BAD_BUFFER;
        }
        bool ubwc_enabled = gralloc::IsUBwcEnabled(format, usage);
        hidl_cb(err, static_cast<int32_t>(alignedw), static_cast<int32_t>(alignedh), ubwc_enabled);
        return Void();
    }

    Return<void> getColorSpace(void* buffer, getColorSpace_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        int color_space = 0;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            gralloc::GetColorSpaceFromMetadata(hnd, &color_space);
            err = qext10::Error::NONE;
        }
        hidl_cb(err, color_space);
        return Void();
    }

    Return<void> getYuvPlaneInfo(void* buffer, getYuvPlaneInfo_cb hidl_cb) override {
        auto err = qext10::Error::BAD_BUFFER;
        hidl_vec<qext10::YCbCrLayout> layout(2);
        auto* hnd = static_cast<private_handle_t*>(buffer);
        android_ycbcr yuv_plane_info[2] = {};
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            if (gralloc::GetYUVPlaneInfo(hnd, yuv_plane_info) == 0) {
                err = qext10::Error::NONE;
                for (int i = 0; i < 2; i++) {
                    layout[i].y = yuv_plane_info[i].y;
                    layout[i].cr = yuv_plane_info[i].cr;
                    layout[i].cb = yuv_plane_info[i].cb;
                    layout[i].yStride = static_cast<uint32_t>(yuv_plane_info[i].ystride);
                    layout[i].cStride = static_cast<uint32_t>(yuv_plane_info[i].cstride);
                    layout[i].chromaStep = static_cast<uint32_t>(yuv_plane_info[i].chroma_step);
                }
            }
        }
        hidl_cb(err, layout);
        return Void();
    }

    Return<qext10::Error> setSingleBufferMode(void* buffer, bool enable) override {
        auto err = qext10::Error::BAD_BUFFER;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            err = static_cast<qext10::Error>(
                    gralloc::SetMetaData(hnd, QTI_SINGLE_BUFFER_MODE, &enable));
            if (err != qext10::Error::NONE) {
                err = qext10::Error::UNSUPPORTED;
            }
        }
        return err;
    }

    Return<void> getCustomFormatFlags(int32_t format, uint64_t usage,
                                      getCustomFormatFlags_cb hidl_cb) override {
        uint64_t priv_flags = 0;
        int32_t custom_format = format;
        auto err = qext10::Error::NONE;
        if (gralloc::GetCustomFormatFlags(format, usage, &custom_format, &priv_flags) != 0) {
            err = qext10::Error::UNSUPPORTED;
        }
        hidl_cb(err, custom_format, priv_flags);
        return Void();
    }

    Return<void> getFd(void* buffer, getFd_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->fd);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getWidth(void* buffer, getWidth_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->width);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getHeight(void* buffer, getHeight_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->height);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getOffset(void* buffer, getOffset_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->offset);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getSize(void* buffer, getSize_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->size);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getUnalignedWidth(void* buffer, getUnalignedWidth_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->unaligned_width);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getUnalignedHeight(void* buffer, getUnalignedHeight_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->unaligned_height);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getLayerCount(void* buffer, getLayerCount_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->layer_count);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getId(void* buffer, getId_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->id);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getUsageFlags(void* buffer, getUsageFlags_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->usage);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getFormat(void* buffer, getFormat_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->format);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getPrivateFlags(void* buffer, getPrivateFlags_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            hidl_cb(qext10::Error::NONE, hnd->flags);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, 0);
        }
        return Void();
    }

    Return<void> getSurfaceMetadata(void* buffer, getSurfaceMetadata_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        GraphicsMetadata surface_metadata = {};
        auto err = qext10::Error::BAD_BUFFER;
        if (hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            err = static_cast<qext10::Error>(
                    gralloc::GetMetaDataValue(hnd, QTI_GRAPHICS_METADATA, &surface_metadata));
        }
        hidl_cb(err, err == qext10::Error::NONE ? &surface_metadata : nullptr);
        return Void();
    }

    Return<void> getFormatLayout(int32_t format, uint64_t usage, int32_t flags, int32_t width,
                                 int32_t height, getFormatLayout_cb hidl_cb) override {
        auto err = qext10::Error::NONE;
        hidl_vec<qext10::PlaneLayout> plane_info;
        unsigned int alignedw = 0;
        unsigned int alignedh = 0;
        int plane_count = 0;
        uint32_t size = 0;

        int custom_format = gralloc::GetImplDefinedFormat(usage, format);
        BufferInfo info(width, height, custom_format, usage);

        if (gralloc::GetBufferSizeAndDimensions(info, &size, &alignedw, &alignedh) != 0) {
            hidl_cb(qext10::Error::BAD_BUFFER, 0, plane_info);
            return Void();
        }

        gralloc::PlaneLayoutInfo plane_layout[8] = {};
        if (gralloc::IsYuvFormat(custom_format)) {
            gralloc::GetYUVPlaneInfo(info, custom_format, alignedw, alignedh, flags, &plane_count,
                                     plane_layout);
        } else if (gralloc::IsUncompressedRGBFormat(custom_format) ||
                   gralloc::IsCompressedRGBFormat(custom_format)) {
            gralloc::GetRGBPlaneInfo(info, custom_format, alignedw, alignedh, flags, &plane_count,
                                     plane_layout);
        } else {
            hidl_cb(qext10::Error::BAD_BUFFER, size, plane_info);
            return Void();
        }

        plane_info.resize(plane_count);
        for (int i = 0; i < plane_count; i++) {
            plane_info[i].component = plane_layout[i].component;
            plane_info[i].h_subsampling = plane_layout[i].h_subsampling;
            plane_info[i].v_subsampling = plane_layout[i].v_subsampling;
            plane_info[i].offset = plane_layout[i].offset;
            plane_info[i].pixel_increment = plane_layout[i].step;
            plane_info[i].stride = plane_layout[i].stride;
            plane_info[i].stride_bytes = plane_layout[i].stride_bytes;
            plane_info[i].scanlines = plane_layout[i].scanlines;
            plane_info[i].size = plane_layout[i].size;
        }
        hidl_cb(err, size, plane_info);
        return Void();
    }

    Return<qext10::Error> getSurfaceMetadata_V1(void* buffer, void* metadata) override {
        auto err = qext10::Error::BAD_BUFFER;
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (metadata != nullptr && hnd != nullptr && private_handle_t::validate(hnd) == 0) {
            err = static_cast<qext10::Error>(
                    gralloc::GetMetaDataValue(hnd, QTI_GRAPHICS_METADATA, metadata));
            if (err != qext10::Error::NONE) {
                err = qext10::Error::UNSUPPORTED;
            }
        }
        return err;
    }

  private:
    BufferManager* buf_mgr_ = nullptr;
};

class ShimQtiMapper3 : public qmapper3::IQtiMapper {
  public:
    ShimQtiMapper3()
        : extensions_(new ShimQtiMapperExtensions()), buf_mgr_(BufferManager::GetInstance()) {}

    Return<void> createDescriptor(const mapper3::IMapper::BufferDescriptorInfo& description,
                                  createDescriptor_cb hidl_cb) override {
        mapper3::BufferDescriptor descriptor;
        if (description.width == 0 || description.height == 0 ||
            static_cast<int32_t>(description.format) <= 0 || description.layerCount == 0) {
            hidl_cb(mapper3::Error::BAD_VALUE, descriptor);
            return Void();
        }
        descriptor.resize(gralloc::kBufferDescriptorSize);
        descriptor[0] = gralloc::kMagicVersion;
        descriptor[1] = description.width;
        descriptor[2] = description.height;
        descriptor[3] = static_cast<uint32_t>(description.format);
        descriptor[4] = description.layerCount;
        descriptor[5] = static_cast<uint32_t>(description.usage & 0xffffffffULL);
        descriptor[6] = static_cast<uint32_t>((description.usage >> 32) & 0xffffffffULL);
        hidl_cb(mapper3::Error::NONE, descriptor);
        return Void();
    }

    Return<void> importBuffer(const hidl_handle& raw_handle, importBuffer_cb hidl_cb) override {
        if (!raw_handle.getNativeHandle()) {
            hidl_cb(mapper3::Error::BAD_BUFFER, nullptr);
            return Void();
        }
        native_handle_t* buffer_handle = native_handle_clone(raw_handle.getNativeHandle());
        if (!buffer_handle) {
            hidl_cb(mapper3::Error::NO_RESOURCES, nullptr);
            return Void();
        }
        auto err = toMapper3Error(buf_mgr_->RetainBuffer(QTI_HANDLE_CONST(buffer_handle)));
        if (err != mapper3::Error::NONE) {
            native_handle_close(buffer_handle);
            native_handle_delete(buffer_handle);
            hidl_cb(err, nullptr);
            return Void();
        }
        hidl_cb(mapper3::Error::NONE, buffer_handle);
        return Void();
    }

    Return<mapper3::Error> freeBuffer(void* buffer) override {
        if (!buffer) {
            return mapper3::Error::BAD_BUFFER;
        }
        return toMapper3Error(buf_mgr_->ReleaseBuffer(QTI_HANDLE_CONST(buffer)));
    }

    Return<mapper3::Error> validateBufferSize(
            void* buffer, const mapper3::IMapper::BufferDescriptorInfo& description,
            uint32_t /*stride*/) override {
        if (!buffer) {
            return mapper3::Error::BAD_BUFFER;
        }
        BufferInfo info(static_cast<int>(description.width), static_cast<int>(description.height),
                        static_cast<int>(description.format),
                        static_cast<uint64_t>(description.usage));
        info.layer_count = description.layerCount;
        return toMapper3Error(buf_mgr_->ValidateBufferSize(QTI_HANDLE_CONST(buffer), info));
    }

    Return<void> getTransportSize(void* buffer, getTransportSize_cb hidl_cb) override {
        auto* hnd = static_cast<private_handle_t*>(buffer);
        if (!hnd || private_handle_t::validate(hnd) != 0) {
            hidl_cb(mapper3::Error::BAD_BUFFER, 0, 0);
            return Void();
        }
        hidl_cb(mapper3::Error::NONE, static_cast<uint32_t>(hnd->numFds),
                static_cast<uint32_t>(hnd->numInts));
        return Void();
    }

    Return<void> lock(void* buffer, uint64_t cpu_usage,
                      const mapper3::IMapper::Rect& access_region,
                      const hidl_handle& acquire_fence, lock_cb hidl_cb) override {
        auto err = lockBufferInternal(buffer, cpu_usage, access_region, acquire_fence);
        if (err != mapper3::Error::NONE) {
            hidl_cb(err, nullptr, -1, -1);
            return Void();
        }
        auto* hnd = QTI_HANDLE_CONST(buffer);
        int32_t bpp = gralloc::GetBpp(hnd->format);
        int32_t stride_bytes = (bpp > 0) ? (hnd->width * bpp) : -1;
        hidl_cb(mapper3::Error::NONE, reinterpret_cast<void*>(hnd->base), bpp, stride_bytes);
        return Void();
    }

    Return<void> lockYCbCr(void* buffer, uint64_t cpu_usage,
                           const mapper3::IMapper::Rect& access_region,
                           const hidl_handle& acquire_fence, lockYCbCr_cb hidl_cb) override {
        mapper3::YCbCrLayout out_layout = {};
        auto err = lockBufferInternal(buffer, cpu_usage, access_region, acquire_fence);
        if (err != mapper3::Error::NONE) {
            hidl_cb(err, out_layout);
            return Void();
        }
        auto* hnd = const_cast<private_handle_t*>(QTI_HANDLE_CONST(buffer));
        android_ycbcr yuv[2] = {};
        if (gralloc::GetYUVPlaneInfo(hnd, yuv) != 0) {
            buf_mgr_->UnlockBuffer(hnd);
            hidl_cb(mapper3::Error::BAD_BUFFER, out_layout);
            return Void();
        }
        out_layout.y = yuv[0].y;
        out_layout.cb = yuv[0].cb;
        out_layout.cr = yuv[0].cr;
        out_layout.yStride = static_cast<uint32_t>(yuv[0].ystride);
        out_layout.cStride = static_cast<uint32_t>(yuv[0].cstride);
        out_layout.chromaStep = static_cast<uint32_t>(yuv[0].chroma_step);
        hidl_cb(mapper3::Error::NONE, out_layout);
        return Void();
    }

    Return<void> unlock(void* buffer, unlock_cb hidl_cb) override {
        if (!buffer) {
            hidl_cb(mapper3::Error::BAD_BUFFER, hidl_handle(nullptr));
            return Void();
        }
        auto err = toMapper3Error(buf_mgr_->UnlockBuffer(QTI_HANDLE_CONST(buffer)));
        hidl_cb(err, hidl_handle(nullptr));
        return Void();
    }

    Return<void> isSupported(const mapper3::IMapper::BufferDescriptorInfo& description,
                             isSupported_cb hidl_cb) override {
        bool supported = description.width > 0 && description.height > 0 &&
                         description.layerCount > 0 && static_cast<int32_t>(description.format) > 0;
        hidl_cb(mapper3::Error::NONE, supported);
        return Void();
    }

    Return<void> getMapperExtensions(getMapperExtensions_cb hidl_cb) override {
        hidl_cb(mapper3::Error::NONE, extensions_);
        return Void();
    }

  private:
    mapper3::Error lockBufferInternal(void* buffer, uint64_t usage,
                                      const mapper3::IMapper::Rect& access_region,
                                      const hidl_handle& acquire_fence) {
        if (!buffer) {
            return mapper3::Error::BAD_BUFFER;
        }
        const native_handle_t* fence = acquire_fence.getNativeHandle();
        if (fence && fence->numFds > 1) {
            return mapper3::Error::BAD_VALUE;
        }
        int fence_fd = (fence && fence->numFds == 1) ? fence->data[0] : -1;
        if (fence_fd >= 0) {
            sync_wait(fence_fd, 3000);
        }
        auto* hnd = QTI_HANDLE_CONST(buffer);
        if (access_region.top < 0 || access_region.left < 0 || access_region.width < 0 ||
            access_region.height < 0 || access_region.width > hnd->width ||
            access_region.height > hnd->height) {
            return mapper3::Error::BAD_VALUE;
        }
        return toMapper3Error(buf_mgr_->LockBuffer(hnd, usage));
    }

    sp<qext10::IQtiMapperExtensions> extensions_;
    BufferManager* buf_mgr_ = nullptr;
};

std::mutex gShimLock;
sp<ShimQtiMapper3> gShimMapper3;
sp<postproc10::IPostProcService> gLocalPostProc;

sp<ShimQtiMapper3> getShimMapper3() {
    std::lock_guard<std::mutex> lock(gShimLock);
    if (!gShimMapper3) {
        gShimMapper3 = new ShimQtiMapper3();
        LOG(INFO) << "Initialized in-process ShimQtiMapper3 + ShimQtiMapperExtensions";
    }
    return gShimMapper3;
}

}  // namespace

// Interpose IMapper@3.0::getService(const std::string&, bool)
sp<mapper3::IMapper> shimMapper3GetService(const std::string& name, bool getStub) __asm__(
        "_ZN7android8hardware8graphics6mapper4V3_07IMapper10getServiceERKNSt3__112basic_stringIcNS5_"
        "11char_traitsIcEENS5_9allocatorIcEEEEb");

sp<mapper3::IMapper> shimMapper3GetService(const std::string&, bool) {
    return getShimMapper3();
}

// Interpose IQtiMapper@3.0::castFrom(const sp<IMapper@3.0>&, bool)
Return<sp<qmapper3::IQtiMapper>> shimQtiMapper3CastFrom(const sp<mapper3::IMapper>& parent,
                                                        bool emitError) __asm__(
        "_ZN6vendor3qti8hardware7display6mapper4V3_010IQtiMapper8castFromERKN7android2spINS6_"
        "8hardware8graphics6mapper4V3_07IMapperEEEb");

Return<sp<qmapper3::IQtiMapper>> shimQtiMapper3CastFrom(const sp<mapper3::IMapper>& parent,
                                                        bool /*emitError*/) {
    if (parent != nullptr) {
        return sp<qmapper3::IQtiMapper>(getShimMapper3());
    }
    return sp<qmapper3::IQtiMapper>(nullptr);
}

// Interpose IMapper@2.0::getService(const std::string&, bool) -> nullptr so callers prefer V3_0
sp<mapper2::IMapper> shimMapper2GetService(const std::string& name, bool getStub) __asm__(
        "_ZN7android8hardware8graphics6mapper4V2_07IMapper10getServiceERKNSt3__112basic_stringIcNS5_"
        "11char_traitsIcEENS5_9allocatorIcEEEEb");

sp<mapper2::IMapper> shimMapper2GetService(const std::string&, bool) {
    return nullptr;
}

// Interpose IPostProcService@1.0::registerAsService / getService / tryGetService
status_t shimPostProcRegister(postproc10::IPostProcService* self, const std::string& name) __asm__(
        "_ZN6vendor3qti8hardware6camera8postproc4V1_016IPostProcService17registerAsServiceERKNSt3__"
        "112basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEE");

status_t shimPostProcRegister(postproc10::IPostProcService* self, const std::string& name) {
    std::lock_guard<std::mutex> lock(gShimLock);
    gLocalPostProc = self;
    LOG(INFO) << "Retained IPostProcService@1.0 (" << name << ") in-process";
    return android::OK;
}

sp<postproc10::IPostProcService> shimPostProcGetService(const std::string& name,
                                                        bool getStub) __asm__(
        "_ZN6vendor3qti8hardware6camera8postproc4V1_016IPostProcService10getServiceERKNSt3__112"
        "basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEEb");

sp<postproc10::IPostProcService> shimPostProcGetService(const std::string&, bool) {
    std::lock_guard<std::mutex> lock(gShimLock);
    return gLocalPostProc;
}

sp<postproc10::IPostProcService> shimPostProcTryGetService(const std::string& name,
                                                           bool getStub) __asm__(
        "_ZN6vendor3qti8hardware6camera8postproc4V1_016IPostProcService13tryGetServiceERKNSt3__112"
        "basic_stringIcNS6_11char_traitsIcEENS6_9allocatorIcEEEEb");

sp<postproc10::IPostProcService> shimPostProcTryGetService(const std::string&, bool) {
    std::lock_guard<std::mutex> lock(gShimLock);
    return gLocalPostProc;
}
