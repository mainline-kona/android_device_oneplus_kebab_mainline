/*
 * Copyright (C) 2020 The Android Open Source Project
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

#define LOG_TAG "GnssPsdsAidl"

#include "GnssPsds.h"
#include <aidl/android/hardware/gnss/BnGnss.h>
#include <errno.h>
#include <fcntl.h>
#include <log/log.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utils/SystemClock.h>
#include <string>

namespace aidl::android::hardware::gnss {

namespace {
constexpr char kHelperPath[] = "/vendor/bin/kona-gnss";
constexpr size_t kMaxPsdsSize = 512 * 1024;
constexpr int64_t kPsdsRefreshIntervalNs = 24LL * 60 * 60 * 1000000000LL;
constexpr int64_t kPsdsRetryIntervalNs = 5LL * 60 * 1000000000LL;
}  // namespace

std::shared_ptr<IGnssPsdsCallback> GnssPsds::sCallback = nullptr;
int64_t GnssPsds::sLastInjectedNs = 0;
int64_t GnssPsds::sLastRequestNs = 0;
std::mutex GnssPsds::sMutex;

ndk::ScopedAStatus GnssPsds::setCallback(const std::shared_ptr<IGnssPsdsCallback>& callback) {
    ALOGD("setCallback");
    std::lock_guard<std::mutex> lock(sMutex);
    sCallback = callback;
    return ndk::ScopedAStatus::ok();
}

void GnssPsds::resetInjectionState() {
    std::lock_guard<std::mutex> lock(sMutex);
    sLastInjectedNs = 0;
    sLastRequestNs = 0;
}

void GnssPsds::requestPsds(PsdsType psdsType) {
    std::shared_ptr<IGnssPsdsCallback> callback;
    int64_t nowNs = ::android::elapsedRealtimeNano();
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sCallback == nullptr) {
            return;
        }
        if (sLastInjectedNs > 0 && (nowNs - sLastInjectedNs) < kPsdsRefreshIntervalNs) {
            return;
        }
        if (sLastRequestNs > 0 && (nowNs - sLastRequestNs) < kPsdsRetryIntervalNs) {
            return;
        }
        sLastRequestNs = nowNs;
        callback = sCallback;
    }

    ALOGI("Requesting PSDS download (type=%d)", static_cast<int>(psdsType));
    auto status = callback->downloadRequestCb(psdsType);
    if (!status.isOk()) {
        ALOGE("downloadRequestCb failed: %s", status.getDescription().c_str());
    }
}

ndk::ScopedAStatus GnssPsds::injectPsdsData(PsdsType psdsType,
                                            const std::vector<uint8_t>& psdsData) {
    ALOGI("injectPsdsData: type=%d, size=%zu bytes", static_cast<int>(psdsType), psdsData.size());
    if (psdsData.empty() || psdsData.size() > kMaxPsdsSize) {
        return ndk::ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_INVALID_ARGUMENT);
    }

    static std::once_flag sigpipeFlag;
    std::call_once(sigpipeFlag, []() { signal(SIGPIPE, SIG_IGN); });

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (pipe2(inPipe, O_CLOEXEC) < 0) {
        ALOGE("pipe2(in) failed: %d", errno);
        return ndk::ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_GENERIC);
    }
    if (pipe2(outPipe, O_CLOEXEC) < 0) {
        ALOGE("pipe2(out) failed: %d", errno);
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        return ndk::ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_GENERIC);
    }

    pid_t pid = fork();
    if (pid < 0) {
        ALOGE("fork failed: %d", errno);
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return ndk::ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_GENERIC);
    }

    if (pid == 0) {
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(outPipe[1], STDERR_FILENO);
        execl(kHelperPath, kHelperPath, "--inject-xtra", "-", (char*)nullptr);
        _exit(127);
    }

    ::close(inPipe[0]);
    ::close(outPipe[1]);

    size_t written = 0;
    bool writeOk = true;
    while (written < psdsData.size()) {
        ssize_t n = write(inPipe[1], psdsData.data() + written, psdsData.size() - written);
        if (n < 0) {
            if (errno == EINTR) continue;
            ALOGE("write to kona-gnss failed at %zu/%zu: %d", written, psdsData.size(), errno);
            writeOk = false;
            break;
        }
        written += static_cast<size_t>(n);
    }
    ::close(inPipe[1]);

    std::string output;
    char buf[512];
    ssize_t n;
    while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) {
        output.append(buf, static_cast<size_t>(n));
    }
    ::close(outPipe[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }

    if (!output.empty()) {
        ALOGI("kona-gnss --inject-xtra: %s", output.c_str());
    }

    if (writeOk && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        std::lock_guard<std::mutex> lock(sMutex);
        sLastInjectedNs = ::android::elapsedRealtimeNano();
        return ndk::ScopedAStatus::ok();
    }

    ALOGE("kona-gnss --inject-xtra failed (status=0x%x)", status);
    return ndk::ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_GENERIC);
}

}  // namespace aidl::android::hardware::gnss
