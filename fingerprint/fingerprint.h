/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_INCLUDE_HARDWARE_FINGERPRINT_H
#define ANDROID_INCLUDE_HARDWARE_FINGERPRINT_H

#include <hardware/hardware.h>
#include <hardware/hw_auth_token.h>
#include <stddef.h>

#define FINGERPRINT_MODULE_API_VERSION_1_0 HARDWARE_MODULE_API_VERSION(1, 0)
#define FINGERPRINT_MODULE_API_VERSION_2_0 HARDWARE_MODULE_API_VERSION(2, 0)
#define FINGERPRINT_MODULE_API_VERSION_2_1 HARDWARE_MODULE_API_VERSION(2, 1)
#define FINGERPRINT_MODULE_API_VERSION_3_0 HARDWARE_MODULE_API_VERSION(3, 0)
#define FINGERPRINT_HARDWARE_MODULE_ID "fingerprint"

typedef enum fingerprint_msg_type {
    FINGERPRINT_ERROR = -1,
    FINGERPRINT_ACQUIRED = 1,
    FINGERPRINT_TEMPLATE_ENROLLING = 3,
    FINGERPRINT_TEMPLATE_REMOVED = 4,
    FINGERPRINT_AUTHENTICATED = 5,
    FINGERPRINT_TEMPLATE_ENUMERATING = 6,
} fingerprint_msg_type_t;

typedef enum fingerprint_error {
    FINGERPRINT_ERROR_HW_UNAVAILABLE = 1,
    FINGERPRINT_ERROR_UNABLE_TO_PROCESS = 2,
    FINGERPRINT_ERROR_TIMEOUT = 3,
    FINGERPRINT_ERROR_NO_SPACE = 4,
    FINGERPRINT_ERROR_CANCELED = 5,
    FINGERPRINT_ERROR_UNABLE_TO_REMOVE = 6,
    FINGERPRINT_ERROR_LOCKOUT = 7,
    FINGERPRINT_ERROR_VENDOR_BASE = 1000
} fingerprint_error_t;

typedef enum fingerprint_acquired_info {
    FINGERPRINT_ACQUIRED_GOOD = 0,
    FINGERPRINT_ACQUIRED_PARTIAL = 1,
    FINGERPRINT_ACQUIRED_INSUFFICIENT = 2,
    FINGERPRINT_ACQUIRED_IMAGER_DIRTY = 3,
    FINGERPRINT_ACQUIRED_TOO_SLOW = 4,
    FINGERPRINT_ACQUIRED_TOO_FAST = 5,
    FINGERPRINT_ACQUIRED_DETECTED = 6,
    FINGERPRINT_ACQUIRED_VENDOR_BASE = 1000
} fingerprint_acquired_info_t;

typedef struct fingerprint_finger_id {
    uint32_t gid;
    uint32_t fid;
} fingerprint_finger_id_t;

typedef struct fingerprint_enroll {
    fingerprint_finger_id_t finger;
    uint32_t samples_remaining;
    uint64_t msg;
} fingerprint_enroll_t;

typedef struct fingerprint_iterator {
    fingerprint_finger_id_t finger;
    uint32_t remaining_templates;
} fingerprint_iterator_t;

typedef fingerprint_iterator_t fingerprint_enumerated_t;
typedef fingerprint_iterator_t fingerprint_removed_t;

typedef struct fingerprint_acquired {
    fingerprint_acquired_info_t acquired_info;
} fingerprint_acquired_t;

typedef struct fingerprint_authenticated {
    fingerprint_finger_id_t finger;
    hw_auth_token_t hat;
} fingerprint_authenticated_t;

typedef struct fingerprint_msg {
    fingerprint_msg_type_t type;
    union {
        fingerprint_error_t error;
        fingerprint_enroll_t enroll;
        fingerprint_enumerated_t enumerated;
        fingerprint_removed_t removed;
        fingerprint_acquired_t acquired;
        fingerprint_authenticated_t authenticated;
    } data;
} fingerprint_msg_t;

typedef void (*fingerprint_notify_t)(const fingerprint_msg_t* msg);

typedef struct fingerprint_device {
    struct hw_device_t common;                                                         // 0x000
    fingerprint_notify_t notify;                                                       // 0x078
    int (*set_notify)(struct fingerprint_device* dev, fingerprint_notify_t notify);    // 0x080
    uint64_t (*pre_enroll)(struct fingerprint_device* dev);                            // 0x088
    int (*enroll)(struct fingerprint_device* dev, const hw_auth_token_t* hat,          // 0x090
                  uint32_t gid, uint32_t timeout_sec);
    int (*post_enroll)(struct fingerprint_device* dev);                                // 0x098
    uint64_t (*get_authenticator_id)(struct fingerprint_device* dev);                  // 0x0a0
    int (*cancel)(struct fingerprint_device* dev);                                     // 0x0a8
    int (*enumerate)(struct fingerprint_device* dev);                                  // 0x0b0
    int (*remove)(struct fingerprint_device* dev, uint32_t gid, uint32_t fid);         // 0x0b8
    int (*set_active_group)(struct fingerprint_device* dev, uint32_t gid,              // 0x0c0
                            const char* store_path);
    int (*authenticate)(struct fingerprint_device* dev, uint64_t operation_id,         // 0x0c8
                        uint32_t gid);

    /* OPlus Goodix optical HAL extensions (verified against fingerprint.19805_goodix9638_G6_3) */
    void* oplus_pad0[5];                                                               // 0x0d0..0x0f0
    int (*pause_enroll)(struct fingerprint_device* dev);                               // 0x0f8
    int (*continue_enroll)(struct fingerprint_device* dev);                            // 0x100
    void* oplus_pad1[6];                                                               // 0x108..0x130
    int (*set_screen_state)(struct fingerprint_device* dev, uint32_t state);           // 0x138
    void* oplus_pad2[2];                                                               // 0x140..0x148
    int (*send_fingerprint_cmd)(struct fingerprint_device* dev, int32_t cmd_id,        // 0x150
                                int8_t* in_buf, uint32_t size);
    void* oplus_pad3[2];                                                               // 0x158..0x160
    int (*set_icon_hide_status)(struct fingerprint_device* dev, int32_t hide);         // 0x168
    void* reserved[4];                                                                 // 0x170..0x188
} fingerprint_device_t;

#ifdef __cplusplus
static_assert(offsetof(fingerprint_device_t, set_notify) == 0x80, "set_notify offset");
static_assert(offsetof(fingerprint_device_t, authenticate) == 0xc8, "authenticate offset");
static_assert(offsetof(fingerprint_device_t, set_screen_state) == 0x138, "set_screen_state offset");
static_assert(offsetof(fingerprint_device_t, send_fingerprint_cmd) == 0x150, "send_fingerprint_cmd offset");
static_assert(offsetof(fingerprint_device_t, set_icon_hide_status) == 0x168, "set_icon_hide_status offset");
#endif

typedef struct fingerprint_module {
    struct hw_module_t common;
} fingerprint_module_t;

#endif /* ANDROID_INCLUDE_HARDWARE_FINGERPRINT_H */
