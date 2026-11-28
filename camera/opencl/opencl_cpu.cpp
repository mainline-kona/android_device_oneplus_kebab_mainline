/*
 * Copyright (C) 2026 The PenguinOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "ClCpu"

#include <CL/cl.h>
#include <CL/cl_ext.h>
#include <log/log.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#define CL_QCOM_NV12 0x4133
#define CL_QCOM_NV12_Y 0x4134
#define CL_QCOM_NV12_UV 0x4135

struct _cl_platform_id {};
struct _cl_device_id {};

struct RefCounted {
    std::atomic<int> refs{1};
};

struct _cl_context : RefCounted {};
struct _cl_command_queue : RefCounted {};
struct _cl_sampler : RefCounted {
    cl_bool normalized;
    cl_addressing_mode addressing;
};
struct _cl_event : RefCounted {};

struct _cl_mem : RefCounted {
    bool image = false;
    uint8_t* base = nullptr;
    size_t size = 0;
    size_t extent = 0;
    void* mapping = nullptr;
    size_t mappingSize = 0;
    std::vector<uint8_t> owned;
    cl_mem parent = nullptr;
    cl_image_format format{};
    size_t width = 0;
    size_t height = 0;
    size_t rowPitch = 0;
    size_t channels = 1;
    size_t channelBytes = 1;
};

struct _cl_program : RefCounted {
    std::string source;
};

struct Arg {
    std::vector<uint8_t> bytes;
};

struct _cl_kernel : RefCounted {
    std::string name;
    std::map<cl_uint, Arg> args;
};

static _cl_platform_id gPlatform;
static _cl_device_id gDevice;

static size_t channelCount(cl_channel_order order) {
    switch (order) {
        case CL_R:
        case CL_A:
        case CL_INTENSITY:
        case CL_LUMINANCE:
        case CL_QCOM_NV12_Y:
            return 1;
        case CL_RG:
        case CL_RA:
        case CL_QCOM_NV12_UV:
            return 2;
        case CL_RGB:
            return 3;
        case CL_RGBA:
        case CL_BGRA:
        case CL_ARGB:
            return 4;
        default:
            ALOGW("Unknown channel order 0x%x", order);
            return 1;
    }
}

static size_t channelSize(cl_channel_type type) {
    switch (type) {
        case CL_UNORM_INT8:
        case CL_SNORM_INT8:
        case CL_SIGNED_INT8:
        case CL_UNSIGNED_INT8:
            return 1;
        case CL_UNORM_INT16:
        case CL_SNORM_INT16:
        case CL_SIGNED_INT16:
        case CL_UNSIGNED_INT16:
        case CL_HALF_FLOAT:
            return 2;
        case CL_FLOAT:
        case CL_SIGNED_INT32:
        case CL_UNSIGNED_INT32:
            return 4;
        default:
            ALOGW("Unknown channel type 0x%x", type);
            return 1;
    }
}

static uint8_t* resolveHostPtr(cl_mem_flags flags, void* hostPtr, size_t size, cl_mem mem) {
    if (!hostPtr) return nullptr;
    if (!(flags & CL_MEM_EXT_HOST_PTR_QCOM)) return static_cast<uint8_t*>(hostPtr);

    auto* ext = static_cast<cl_mem_ext_host_ptr*>(hostPtr);
    if (ext->allocation_type == CL_MEM_ION_HOST_PTR_QCOM) {
        auto* ion = static_cast<cl_mem_ion_host_ptr*>(hostPtr);
        // Kernels address the dma-buf from its start, like the GPU mapping does; the host
        // pointer can sit at a plane further into the buffer
        off_t bufSize = lseek(ion->ion_filedesc, 0, SEEK_END);
        if (bufSize > 0) mem->extent = std::max(size, static_cast<size_t>(bufSize));
        size_t mapSize = std::max(size, mem->extent);
        void* map = mmap(nullptr, mapSize, PROT_READ | PROT_WRITE, MAP_SHARED, ion->ion_filedesc, 0);
        if (map == MAP_FAILED) {
            ALOGE("Failed to map fd %d (%zu bytes)", ion->ion_filedesc, mapSize);
            return nullptr;
        }
        mem->mapping = map;
        mem->mappingSize = mapSize;
        return static_cast<uint8_t*>(map);
    }
    ALOGE("Unsupported ext host ptr type 0x%x", ext->allocation_type);
    return nullptr;
}

template <typename T>
static bool valid(T* obj) {
    return obj != nullptr;
}

extern "C" {

cl_int clGetPlatformIDs(cl_uint numEntries, cl_platform_id* platforms, cl_uint* numPlatforms) {
    if (platforms && numEntries > 0) platforms[0] = &gPlatform;
    if (numPlatforms) *numPlatforms = 1;
    return CL_SUCCESS;
}

cl_int clGetDeviceIDs(cl_platform_id, cl_device_type type, cl_uint numEntries,
                      cl_device_id* devices, cl_uint* numDevices) {
    if (!(type & (CL_DEVICE_TYPE_GPU | CL_DEVICE_TYPE_DEFAULT))) return CL_DEVICE_NOT_FOUND;
    if (devices && numEntries > 0) devices[0] = &gDevice;
    if (numDevices) *numDevices = 1;
    return CL_SUCCESS;
}

static cl_int returnInfo(const void* data, size_t size, size_t valueSize, void* value,
                         size_t* sizeRet) {
    if (value) {
        if (valueSize < size) return CL_INVALID_VALUE;
        memcpy(value, data, size);
    }
    if (sizeRet) *sizeRet = size;
    return CL_SUCCESS;
}

cl_int clGetDeviceInfo(cl_device_id, cl_device_info param, size_t valueSize, void* value,
                       size_t* sizeRet) {
    auto str = [&](const char* s) { return returnInfo(s, strlen(s) + 1, valueSize, value, sizeRet); };
    auto num = [&](auto v) { return returnInfo(&v, sizeof(v), valueSize, value, sizeRet); };
    switch (param) {
        case CL_DEVICE_TYPE:
            return num(static_cast<cl_device_type>(CL_DEVICE_TYPE_GPU));
        case CL_DEVICE_NAME:
            return str("QUALCOMM Adreno(TM) 730");
        case CL_DEVICE_VENDOR:
            return str("QUALCOMM");
        case CL_DEVICE_VERSION:
            return str("OpenCL 2.0 Adreno(TM) 730");
        case CL_DRIVER_VERSION:
            return str("OpenCL 2.0 CPU");
        case CL_DEVICE_OPENCL_C_VERSION:
            return str("OpenCL C 2.0 Adreno(TM) 730");
        case CL_DEVICE_EXTENSIONS:
            return str("cl_khr_fp16 cl_khr_3d_image_writes cl_qcom_ext_host_ptr "
                       "cl_qcom_ion_host_ptr cl_qcom_other_image cl_qcom_extract_image_plane "
                       "cl_qcom_android_native_buffer_host_ptr");
        case CL_DEVICE_IMAGE_SUPPORT:
        case CL_DEVICE_AVAILABLE:
        case CL_DEVICE_COMPILER_AVAILABLE:
            return num(static_cast<cl_bool>(CL_TRUE));
        case CL_DEVICE_IMAGE2D_MAX_WIDTH:
        case CL_DEVICE_IMAGE2D_MAX_HEIGHT:
            return num(static_cast<size_t>(16384));
        case CL_DEVICE_MAX_WORK_GROUP_SIZE:
            return num(static_cast<size_t>(1024));
        case CL_DEVICE_MAX_COMPUTE_UNITS:
            return num(static_cast<cl_uint>(8));
        case CL_DEVICE_GLOBAL_MEM_SIZE:
        case CL_DEVICE_MAX_MEM_ALLOC_SIZE:
            return num(static_cast<cl_ulong>(1) << 30);
        case CL_DEVICE_PAGE_SIZE_QCOM:
            return num(static_cast<size_t>(4096));
        case CL_DEVICE_EXT_MEM_PADDING_IN_BYTES_QCOM:
            return num(static_cast<size_t>(0));
        default:
            if (value) memset(value, 0, valueSize);
            if (sizeRet) *sizeRet = valueSize;
            return CL_SUCCESS;
    }
}

cl_context clCreateContext(const cl_context_properties*, cl_uint, const cl_device_id*,
                           void(CL_CALLBACK*)(const char*, const void*, size_t, void*), void*,
                           cl_int* err) {
    if (err) *err = CL_SUCCESS;
    return new _cl_context();
}

cl_int clReleaseContext(cl_context ctx) {
    if (!valid(ctx)) return CL_INVALID_CONTEXT;
    if (--ctx->refs == 0) delete ctx;
    return CL_SUCCESS;
}

cl_command_queue clCreateCommandQueue(cl_context, cl_device_id, cl_command_queue_properties,
                                      cl_int* err) {
    if (err) *err = CL_SUCCESS;
    return new _cl_command_queue();
}

cl_int clReleaseCommandQueue(cl_command_queue q) {
    if (!valid(q)) return CL_INVALID_COMMAND_QUEUE;
    if (--q->refs == 0) delete q;
    return CL_SUCCESS;
}

cl_program clCreateProgramWithSource(cl_context, cl_uint count, const char** strings,
                                     const size_t* lengths, cl_int* err) {
    auto* p = new _cl_program();
    for (cl_uint i = 0; i < count; i++) {
        p->source.append(strings[i], lengths && lengths[i] ? lengths[i] : strlen(strings[i]));
    }
    if (err) *err = CL_SUCCESS;
    return p;
}

cl_program clCreateProgramWithBinary(cl_context, cl_uint, const cl_device_id*, const size_t* lengths,
                                     const unsigned char** binaries, cl_int* status, cl_int* err) {
    auto* p = new _cl_program();
    p->source.assign(reinterpret_cast<const char*>(binaries[0]), lengths[0]);
    if (status) status[0] = CL_SUCCESS;
    if (err) *err = CL_SUCCESS;
    return p;
}

cl_int clBuildProgram(cl_program, cl_uint, const cl_device_id*, const char*,
                      void(CL_CALLBACK*)(cl_program, void*), void*) {
    return CL_SUCCESS;
}

cl_int clGetProgramBuildInfo(cl_program, cl_device_id, cl_program_build_info param,
                             size_t valueSize, void* value, size_t* sizeRet) {
    if (param == CL_PROGRAM_BUILD_STATUS) {
        cl_build_status s = CL_BUILD_SUCCESS;
        return returnInfo(&s, sizeof(s), valueSize, value, sizeRet);
    }
    return returnInfo("", 1, valueSize, value, sizeRet);
}

cl_int clGetProgramInfo(cl_program program, cl_program_info param, size_t valueSize, void* value,
                        size_t* sizeRet) {
    switch (param) {
        case CL_PROGRAM_NUM_DEVICES: {
            cl_uint n = 1;
            return returnInfo(&n, sizeof(n), valueSize, value, sizeRet);
        }
        case CL_PROGRAM_DEVICES: {
            cl_device_id d = &gDevice;
            return returnInfo(&d, sizeof(d), valueSize, value, sizeRet);
        }
        case CL_PROGRAM_BINARY_SIZES: {
            size_t s = program->source.size();
            return returnInfo(&s, sizeof(s), valueSize, value, sizeRet);
        }
        case CL_PROGRAM_BINARIES:
            if (value) {
                auto** out = static_cast<unsigned char**>(value);
                if (out[0]) memcpy(out[0], program->source.data(), program->source.size());
            }
            if (sizeRet) *sizeRet = sizeof(unsigned char*);
            return CL_SUCCESS;
        case CL_PROGRAM_SOURCE:
            return returnInfo(program->source.c_str(), program->source.size() + 1, valueSize, value,
                              sizeRet);
        default:
            if (value) memset(value, 0, valueSize);
            if (sizeRet) *sizeRet = valueSize;
            return CL_SUCCESS;
    }
}

cl_int clReleaseProgram(cl_program p) {
    if (!valid(p)) return CL_INVALID_PROGRAM;
    if (--p->refs == 0) delete p;
    return CL_SUCCESS;
}

cl_kernel clCreateKernel(cl_program, const char* name, cl_int* err) {
    auto* k = new _cl_kernel();
    k->name = name;
    if (err) *err = CL_SUCCESS;
    return k;
}

cl_int clReleaseKernel(cl_kernel k) {
    if (!valid(k)) return CL_INVALID_KERNEL;
    if (--k->refs == 0) delete k;
    return CL_SUCCESS;
}

cl_int clSetKernelArg(cl_kernel k, cl_uint index, size_t size, const void* value) {
    Arg a;
    if (value) a.bytes.assign(static_cast<const uint8_t*>(value),
                              static_cast<const uint8_t*>(value) + size);
    else a.bytes.assign(size, 0);
    k->args[index] = std::move(a);
    return CL_SUCCESS;
}

cl_mem clCreateBuffer(cl_context, cl_mem_flags flags, size_t size, void* hostPtr, cl_int* err) {
    auto* m = new _cl_mem();
    m->size = size;
    m->base = resolveHostPtr(flags, hostPtr, size, m);
    if (m->extent > m->size) m->size = m->extent;
    if (!m->base) {
        m->owned.resize(size);
        m->base = m->owned.data();
        if (hostPtr && (flags & CL_MEM_COPY_HOST_PTR)) memcpy(m->base, hostPtr, size);
    }
    if (err) *err = CL_SUCCESS;
    return m;
}

cl_mem clCreateImage(cl_context, cl_mem_flags flags, const cl_image_format* format,
                     const cl_image_desc* desc, void* hostPtr, cl_int* err) {
    auto* m = new _cl_mem();
    m->image = true;
    m->format = *format;
    m->width = desc->image_width;
    m->height = desc->image_height;
    m->channels = channelCount(format->image_channel_order);
    m->channelBytes = channelSize(format->image_channel_data_type);
    m->rowPitch = desc->image_row_pitch ? desc->image_row_pitch
                                        : m->width * m->channels * m->channelBytes;

    if (desc->mem_object) {
        cl_mem parent = desc->mem_object;
        m->parent = parent;
        parent->refs++;
        m->base = parent->base;
        if (!desc->image_row_pitch && parent->image) m->rowPitch = parent->rowPitch;
        if (format->image_channel_order == CL_QCOM_NV12_UV && parent->image) {
            m->base = parent->base + parent->rowPitch * parent->height;
        }
        m->size = m->rowPitch * m->height;
    } else {
        m->size = m->rowPitch * m->height;
        if (format->image_channel_order == CL_QCOM_NV12) m->size = m->size * 3 / 2;
        m->base = resolveHostPtr(flags, hostPtr, m->size, m);
        if (!m->base) {
            m->owned.resize(m->size);
            m->base = m->owned.data();
        }
    }
    ALOGD("image %p order 0x%x type 0x%x %zux%zu pitch %zu parent %p", m,
          format->image_channel_order, format->image_channel_data_type, m->width, m->height,
          m->rowPitch, m->parent);
    if (err) *err = CL_SUCCESS;
    return m;
}

cl_mem clCreateImage2D(cl_context ctx, cl_mem_flags flags, const cl_image_format* format,
                       size_t width, size_t height, size_t pitch, void* hostPtr, cl_int* err) {
    cl_image_desc desc{};
    desc.image_type = CL_MEM_OBJECT_IMAGE2D;
    desc.image_width = width;
    desc.image_height = height;
    desc.image_row_pitch = pitch;
    return clCreateImage(ctx, flags, format, &desc, hostPtr, err);
}

cl_int clReleaseMemObject(cl_mem m) {
    if (!valid(m)) return CL_INVALID_MEM_OBJECT;
    if (--m->refs == 0) {
        if (m->mapping) munmap(m->mapping, m->mappingSize);
        if (m->parent) clReleaseMemObject(m->parent);
        delete m;
    }
    return CL_SUCCESS;
}

cl_sampler clCreateSampler(cl_context, cl_bool normalized, cl_addressing_mode addressing,
                           cl_filter_mode, cl_int* err) {
    auto* s = new _cl_sampler();
    s->normalized = normalized;
    s->addressing = addressing;
    if (err) *err = CL_SUCCESS;
    return s;
}

cl_int clReleaseSampler(cl_sampler s) {
    if (!valid(s)) return CL_INVALID_SAMPLER;
    if (--s->refs == 0) delete s;
    return CL_SUCCESS;
}

}  // extern "C"

template <typename T>
static T argAs(const cl_kernel k, cl_uint i) {
    T v{};
    auto it = k->args.find(i);
    if (it != k->args.end()) memcpy(&v, it->second.bytes.data(), std::min(sizeof(T), it->second.bytes.size()));
    return v;
}

static cl_mem argMem(const cl_kernel k, cl_uint i) {
    return argAs<cl_mem>(k, i);
}

static inline uint8_t* pixel(cl_mem img, long x, long y) {
    return img->base + y * img->rowPitch + x * img->channels * img->channelBytes;
}

static inline long clampl(long v, long lo, long hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void copyPixel(cl_mem dst, long dx, long dy, cl_mem src, long sx, long sy, bool swap) {
    size_t bytes = dst->channels * dst->channelBytes;
    uint8_t* d = pixel(dst, dx, dy);
    const uint8_t* s = pixel(src, sx, sy);
    if (swap && dst->channels >= 2) {
        size_t cb = dst->channelBytes;
        memcpy(d, s + cb, cb);
        memcpy(d + cb, s, cb);
        if (bytes > 2 * cb) memcpy(d + 2 * cb, s + 2 * cb, bytes - 2 * cb);
    } else {
        memcpy(d, s, bytes);
    }
}

static double readChannel(cl_mem img, long x, long y, size_t c) {
    const uint8_t* p = pixel(img, x, y) + c * img->channelBytes;
    switch (img->channelBytes) {
        case 1:
            return *p;
        case 2: {
            uint16_t v;
            memcpy(&v, p, 2);
            return v;
        }
        default: {
            float v;
            memcpy(&v, p, 4);
            return v;
        }
    }
}

static void writeChannel(cl_mem img, long x, long y, size_t c, double v) {
    uint8_t* p = pixel(img, x, y) + c * img->channelBytes;
    switch (img->channelBytes) {
        case 1:
            *p = static_cast<uint8_t>(clampl(lround(v), 0, 255));
            break;
        case 2: {
            uint16_t w = static_cast<uint16_t>(clampl(lround(v), 0, 65535));
            memcpy(p, &w, 2);
            break;
        }
        default: {
            float f = static_cast<float>(v);
            memcpy(p, &f, 4);
        }
    }
}

static void boxFilter(cl_mem dst, long dx, long dy, cl_mem src, float cx, float cy, float bw,
                      float bh, bool swap) {
    long x0 = clampl(lroundf(cx - bw / 2), 0, src->width - 1);
    long x1 = clampl(std::max(lroundf(cx + bw / 2), x0 + 1), 1, src->width);
    long y0 = clampl(lroundf(cy - bh / 2), 0, src->height - 1);
    long y1 = clampl(std::max(lroundf(cy + bh / 2), y0 + 1), 1, src->height);
    double sum[4] = {};
    for (long y = y0; y < y1; y++)
        for (long x = x0; x < x1; x++)
            for (size_t c = 0; c < src->channels && c < 4; c++) sum[c] += readChannel(src, x, y, c);
    double n = static_cast<double>((x1 - x0) * (y1 - y0));
    for (size_t c = 0; c < dst->channels && c < 4; c++) {
        size_t sc = (swap && c < 2) ? 1 - c : c;
        writeChannel(dst, dx, dy, c, sum[sc] / n);
    }
}

template <typename F>
static void parallelRows(size_t width, size_t height, F&& f) {
    unsigned n = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < n; t++) {
        threads.emplace_back([&, t] {
            for (size_t y = t; y < height; y += n)
                for (size_t x = 0; x < width; x++) f(x, y);
        });
    }
    for (auto& th : threads) th.join();
}

static cl_int runKernel(cl_kernel k, size_t gw, size_t gh) {
    const std::string& n = k->name;
    if (n == "copyImage") {
        cl_mem dst = argMem(k, 0), src = argMem(k, 1);
        bool swap = argAs<cl_uint>(k, 2) != 0;
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            if (x < dst->width && y < dst->height && x < src->width && y < src->height)
                copyPixel(dst, x, y, src, x, y, swap);
        });
    } else if (n == "rotateImage") {
        cl_mem dst = argMem(k, 0), src = argMem(k, 1);
        int dw = argAs<cl_int>(k, 2), dh = argAs<cl_int>(k, 3);
        int sw = argAs<cl_int>(k, 4), sh = argAs<cl_int>(k, 5);
        float sinT = argAs<cl_float>(k, 6), cosT = argAs<cl_float>(k, 7);
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            if (x >= dst->width || y >= dst->height) return;
            int tx = static_cast<int>(x) - dw / 2, ty = static_cast<int>(y) - dh / 2;
            long sx = static_cast<long>(tx * cosT + ty * sinT) + sw / 2;
            long sy = static_cast<long>(tx * -sinT + ty * cosT) + sh / 2;
            sx = ((sx % static_cast<long>(src->width)) + src->width) % src->width;
            sy = ((sy % static_cast<long>(src->height)) + src->height) % src->height;
            copyPixel(dst, x, y, src, sx, sy, false);
        });
    } else if (n == "flipImage") {
        cl_mem dst = argMem(k, 0), src = argMem(k, 1);
        int w = argAs<cl_int>(k, 2), h = argAs<cl_int>(k, 3);
        cl_uint inStride = argAs<cl_uint>(k, 4), outStride = argAs<cl_uint>(k, 5);
        cl_uint inUV = argAs<cl_uint>(k, 6), outUV = argAs<cl_uint>(k, 7);
        cl_uint swap = argAs<cl_uint>(k, 8);
        int dir = argAs<cl_int>(k, 9);
        ALOGD("flipImage %dx%d in %u/%u out %u/%u swap %u dir %d src %zu dst %zu", w, h, inStride,
              inUV, outStride, outUV, swap, dir, src->size, dst->size);
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            long sx = x, sy = y;
            if (dir == 1 || dir == 3) sx = w - x - 1;
            if (dir == 2 || dir == 3) sy = h - y - 1;
            if (sx < 0 || sy < 0) return;
            if (inUV == 0) {
                size_t d = y * outStride + x, s = sy * inStride + sx;
                if (d < dst->size && s < src->size) dst->base[d] = src->base[s];
            } else {
                size_t d = y * outStride + x * 2 + outUV, s = sy * inStride + sx * 2 + inUV;
                if (d + 1 >= dst->size || s + 1 >= src->size) return;
                dst->base[d] = src->base[s + (swap == 1 ? 1 : 0)];
                dst->base[d + 1] = src->base[s + (swap == 1 ? 0 : 1)];
            }
        });
    } else if (n == "boxfilter_single_plane") {
        cl_mem src = argMem(k, 0), dst = argMem(k, 1);
        cl_float2 box = argAs<cl_float2>(k, 3), scale = argAs<cl_float2>(k, 4);
        bool swap = argAs<cl_uint>(k, 5) != 0;
        float yMax = argAs<cl_float>(k, 6);
        cl_int2 crop = argAs<cl_int2>(k, 7);
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            if (x >= dst->width || y >= dst->height) return;
            float cx = (x + 0.5f) * scale.s[0] + crop.s[0];
            float cy = std::min((y + 0.5f) * scale.s[1] + crop.s[1], yMax);
            boxFilter(dst, x, y, src, cx, cy, box.s[0], box.s[1], swap);
        });
    } else if (n == "boxfilter_single_plane_twoops") {
        cl_mem src = argMem(k, 0), dst = argMem(k, 1), dst2 = argMem(k, 2);
        cl_float2 box = argAs<cl_float2>(k, 4), box2 = argAs<cl_float2>(k, 5);
        cl_float2 scale = argAs<cl_float2>(k, 6), scale2 = argAs<cl_float2>(k, 7);
        float yMax = argAs<cl_float>(k, 8);
        bool swap = argAs<cl_uint>(k, 13) != 0, swap2 = argAs<cl_uint>(k, 14) != 0;
        cl_int2 crop = argAs<cl_int2>(k, 15), crop2 = argAs<cl_int2>(k, 16);
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            if (x < dst->width && y < dst->height) {
                float cx = (x + 0.5f) * scale.s[0] + crop.s[0];
                float cy = std::min((y + 0.5f) * scale.s[1] + crop.s[1], yMax);
                boxFilter(dst, x, y, src, cx, cy, box.s[0], box.s[1], swap);
            }
            if (x < dst2->width && y < dst2->height) {
                float cx = (x + 0.5f) * scale2.s[0] + crop2.s[0];
                float cy = std::min((y + 0.5f) * scale2.s[1] + crop2.s[1], yMax);
                boxFilter(dst2, x, y, src, cx, cy, box2.s[0], box2.s[1], swap2);
            }
        });
    } else if (n == "ds4_single_plane") {
        cl_mem src = argMem(k, 0), dst = argMem(k, 1);
        float yMax = argAs<cl_float>(k, 4);
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            if (x >= dst->width || y >= dst->height) return;
            boxFilter(dst, x, y, src, x * 4.0f + 2.0f, std::min(y * 4.0f + 2.0f, yMax), 4, 4,
                      false);
        });
    } else if (n == "p010_to_pd10") {
        cl_mem dst = argMem(k, 0), src = argMem(k, 1);
        cl_uint inStride = argAs<cl_uint>(k, 2), inScanline = argAs<cl_uint>(k, 3);
        cl_uint outStride = argAs<cl_uint>(k, 4);
        const uint16_t* luma = reinterpret_cast<const uint16_t*>(src->base);
        const uint16_t* chroma = reinterpret_cast<const uint16_t*>(src->base + inStride * inScanline);
        size_t pitch = inStride / 2;
        parallelRows(gw, gh, [&](size_t x, size_t y) {
            const uint16_t* row = luma + inStride * y;
            const uint16_t* crow = chroma + pitch * y;
            if (reinterpret_cast<const uint8_t*>(crow + 2 * x + 2) > src->base + src->size ||
                reinterpret_cast<const uint8_t*>(row + pitch + 2 * x + 2) > src->base + src->size ||
                outStride * y + x * 8 + 8 > dst->size)
                return;
            uint8_t* out = dst->base + outStride * y + x * 8;
            uint16_t y0 = row[2 * x] >> 6, y1 = row[2 * x + 1] >> 6;
            uint16_t y2 = row[pitch + 2 * x] >> 6, y3 = row[pitch + 2 * x + 1] >> 6;
            uint16_t cb = crow[2 * x] >> 6, cr = crow[2 * x + 1] >> 6;
            out[0] = y0 >> 2;
            out[1] = y2 >> 2;
            out[2] = y1 >> 2;
            out[3] = y3 >> 2;
            out[4] = cb >> 2;
            out[5] = cr >> 2;
            out[6] = (y0 & 3) | ((y2 & 3) << 2) | ((y1 & 3) << 4) | ((y3 & 3) << 6);
            out[7] = (cb & 3) | ((cr & 3) << 2);
        });
    } else {
        ALOGE("Kernel %s isn't implemented", n.c_str());
        return CL_INVALID_KERNEL;
    }
    return CL_SUCCESS;
}

extern "C" {

cl_int clEnqueueNDRangeKernel(cl_command_queue, cl_kernel k, cl_uint dims, const size_t*,
                              const size_t* global, const size_t*, cl_uint, const cl_event*,
                              cl_event* event) {
    size_t gw = global[0], gh = dims > 1 ? global[1] : 1;
    ALOGD("%s %zux%zu", k->name.c_str(), gw, gh);
    cl_int ret = runKernel(k, gw, gh);
    if (event) *event = new _cl_event();
    return ret;
}

cl_int clEnqueueCopyBuffer(cl_command_queue, cl_mem src, cl_mem dst, size_t srcOffset,
                           size_t dstOffset, size_t size, cl_uint, const cl_event*,
                           cl_event* event) {
    memcpy(dst->base + dstOffset, src->base + srcOffset, size);
    if (event) *event = new _cl_event();
    return CL_SUCCESS;
}

cl_int clFlush(cl_command_queue) {
    return CL_SUCCESS;
}

cl_int clFinish(cl_command_queue) {
    return CL_SUCCESS;
}

cl_int clWaitForEvents(cl_uint, const cl_event*) {
    return CL_SUCCESS;
}

cl_int clReleaseEvent(cl_event e) {
    if (!valid(e)) return CL_INVALID_EVENT;
    if (--e->refs == 0) delete e;
    return CL_SUCCESS;
}

cl_int clGetEventProfilingInfo(cl_event, cl_profiling_info, size_t valueSize, void* value,
                               size_t* sizeRet) {
    if (value) memset(value, 0, valueSize);
    if (sizeRet) *sizeRet = sizeof(cl_ulong);
    return CL_SUCCESS;
}

}  // extern "C"
