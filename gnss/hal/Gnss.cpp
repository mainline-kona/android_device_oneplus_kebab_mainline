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

#define LOG_TAG "GnssAidl"

#include "Gnss.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <log/log.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utils/SystemClock.h>
#include <utils/Timers.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>
#include "AGnss.h"
#include "AGnssRil.h"
#include "DeviceFileReader.h"
#include "FixLocationParser.h"
#include "GnssAntennaInfo.h"
#include "GnssAssistanceInterface.h"
#include "GnssBatching.h"
#include "GnssConfiguration.h"
#include "GnssDebug.h"
#include "GnssGeofence.h"
#include "GnssNavigationMessageInterface.h"
#include "GnssPsds.h"
#include "GnssVisibilityControl.h"
#include "MeasurementCorrectionsInterface.h"
#include "Utils.h"

namespace aidl::android::hardware::gnss {
using ::android::hardware::gnss::common::Utils;

using ndk::ScopedAStatus;
using GnssSvInfo = IGnssCallback::GnssSvInfo;

constexpr char kHelperPath[] = "/vendor/bin/kona-gnss";

std::shared_ptr<IGnssCallback> Gnss::sGnssCallback = nullptr;

Gnss::Gnss() : mMinIntervalMs(1000), mFirstFixReceived(false) {}

ScopedAStatus Gnss::setCallback(const std::shared_ptr<IGnssCallback>& callback) {
    ALOGD("setCallback");
    if (callback == nullptr) {
        ALOGE("%s: Null callback ignored", __func__);
        return ScopedAStatus::fromExceptionCode(STATUS_INVALID_OPERATION);
    }
    {
        std::unique_lock<std::mutex> lock(mMutex);
        sGnssCallback = callback;
    }

    int capabilities = (int)IGnssCallback::CAPABILITY_SCHEDULING |
                       (int)IGnssCallback::CAPABILITY_ON_DEMAND_TIME;
    auto status = callback->gnssSetCapabilitiesCb(capabilities);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke callback.gnssSetCapabilitiesCb", __func__);
    }

    IGnssCallback::GnssSystemInfo systemInfo = {
            .yearOfHw = 2023,
            .name = "Qualcomm modem GNSS (QMI LOC)",
    };
    status = callback->gnssSetSystemInfoCb(systemInfo);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke callback.gnssSetSystemInfoCb", __func__);
    }
    std::vector<GnssSignalType> signalTypes = {
            {.constellation = GnssConstellationType::GPS,
             .carrierFrequencyHz = 1.57542e+09,
             .codeType = GnssSignalType::CODE_TYPE_C},
            {.constellation = GnssConstellationType::GPS,
             .carrierFrequencyHz = 1.17645e+09,
             .codeType = GnssSignalType::CODE_TYPE_Q},
            {.constellation = GnssConstellationType::GLONASS,
             .carrierFrequencyHz = 1.5980625e+09,
             .codeType = GnssSignalType::CODE_TYPE_C},
            {.constellation = GnssConstellationType::GALILEO,
             .carrierFrequencyHz = 1.57542e+09,
             .codeType = GnssSignalType::CODE_TYPE_C},
            {.constellation = GnssConstellationType::GALILEO,
             .carrierFrequencyHz = 1.17645e+09,
             .codeType = GnssSignalType::CODE_TYPE_Q},
            {.constellation = GnssConstellationType::BEIDOU,
             .carrierFrequencyHz = 1.561098e+09,
             .codeType = GnssSignalType::CODE_TYPE_I},
            {.constellation = GnssConstellationType::BEIDOU,
             .carrierFrequencyHz = 1.17645e+09,
             .codeType = GnssSignalType::CODE_TYPE_D},
            {.constellation = GnssConstellationType::QZSS,
             .carrierFrequencyHz = 1.57542e+09,
             .codeType = GnssSignalType::CODE_TYPE_C},
            {.constellation = GnssConstellationType::QZSS,
             .carrierFrequencyHz = 1.17645e+09,
             .codeType = GnssSignalType::CODE_TYPE_Q},
            {.constellation = GnssConstellationType::IRNSS,
             .carrierFrequencyHz = 1.17645e+09,
             .codeType = GnssSignalType::CODE_TYPE_C},
    };
    status = callback->gnssSetSignalTypeCapabilitiesCb(signalTypes);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke callback.gnssSetSignalTypeCapabilitiesCb", __func__);
    }

    // In case when the setCallback() and close() calls are not balanced
    mIsInitialized = false;
    mIsActive = false;
    mThreadBlocker.notify();
    if (mThread.joinable()) {
        mThread.join();
    }

    mIsInitialized = true;
    mThreadBlocker.reset();
    mThread = std::thread([this]() {
        while (mIsInitialized) {
            if (mIsActive && mHelperFd >= 0) {
                readHelper();
            } else {
                // Wait until start() or close() is called
                mThreadBlocker.wait();
            }
        }
    });

    return ScopedAStatus::ok();
}

bool Gnss::startHelper() {
    std::lock_guard<std::mutex> lock(mHelperMutex);
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return false;
    std::string intervalArg = std::to_string(mMinIntervalMs.load());
    pid_t pid = fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        execl(kHelperPath, kHelperPath, "--interval", intervalArg.c_str(), (char*)nullptr);
        _exit(127);
    }
    ::close(fds[1]);
    mHelperPid = pid;
    mHelperFd = fds[0];
    return true;
}

void Gnss::stopHelper() {
    std::lock_guard<std::mutex> lock(mHelperMutex);
    if (mHelperPid > 0) {
        kill(mHelperPid, SIGTERM);
        waitpid(mHelperPid, nullptr, 0);
        mHelperPid = -1;
    }
    if (mHelperFd >= 0) {
        ::close(mHelperFd);
        mHelperFd = -1;
    }
    mPending.clear();
    mSvs.clear();
    mUsedSvs.clear();
}

void Gnss::runHelperOneShot(std::vector<std::string> args) {
    std::thread([args = std::move(args)]() {
        int fds[2];
        if (pipe2(fds, O_CLOEXEC) < 0) return;

        pid_t pid = fork();
        if (pid < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return;
        }
        if (pid == 0) {
            dup2(fds[1], STDOUT_FILENO);
            dup2(fds[1], STDERR_FILENO);
            std::vector<char*> argv;
            argv.reserve(args.size() + 2);
            argv.push_back(const_cast<char*>(kHelperPath));
            for (const auto& a : args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);
            execv(kHelperPath, argv.data());
            _exit(127);
        }
        ::close(fds[1]);

        std::string output;
        char buf[512];
        ssize_t n;
        while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
            output.append(buf, static_cast<size_t>(n));
        }
        ::close(fds[0]);

        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        if (!output.empty()) {
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
                output.pop_back();
            }
            ALOGI("kona-gnss(%s): %s", args.empty() ? "" : args[0].c_str(), output.c_str());
        }
    }).detach();
}

void Gnss::readHelper() {
    int fd = mHelperFd;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 500) <= 0) return;
    char buf[4096];
    ssize_t n;
    {
        // stop() may have closed and replaced the pipe while we polled
        std::lock_guard<std::mutex> lock(mHelperMutex);
        if (fd != mHelperFd) return;
        n = read(fd, buf, sizeof(buf));
    }
    if (n <= 0) {
        ALOGE("kona-gnss exited");
        stopHelper();
        return;
    }
    mPending.append(buf, static_cast<size_t>(n));
    size_t pos;
    while ((pos = mPending.find('\n')) != std::string::npos) {
        std::string line = mPending.substr(0, pos);
        mPending.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("error=", 0) == 0) {
            ALOGE("kona-gnss: %s", line.c_str());
        } else if (line.rfind("warning=", 0) == 0) {
            ALOGW("kona-gnss: %s", line.c_str());
        } else if (line.rfind("pos ", 0) == 0) {
            handlePosition(line);
        } else if (!line.empty() && line[0] == '$') {
            handleSentence(line);
        }
    }
}

static std::vector<std::string> splitFields(const std::string& line) {
    std::string body = line.substr(0, line.find('*'));
    std::vector<std::string> fields;
    std::stringstream ss(body);
    std::string field;
    while (std::getline(ss, field, ',')) fields.push_back(field);
    if (!body.empty() && body.back() == ',') fields.push_back("");
    return fields;
}

static GnssConstellationType constellationOf(const std::string& talker, int* svid) {
    if (talker == "GL") {
        if (*svid > 64) *svid -= 64;
        return GnssConstellationType::GLONASS;
    }
    if (talker == "GA") return GnssConstellationType::GALILEO;
    if (talker == "GB" || talker == "BD") {
        if (*svid > 200) *svid -= 200;
        return GnssConstellationType::BEIDOU;
    }
    if (talker == "GQ") return GnssConstellationType::QZSS;
    if (talker == "GI") return GnssConstellationType::IRNSS;
    if (talker == "GP") return GnssConstellationType::GPS;
    return GnssConstellationType::UNKNOWN;
}

static GnssConstellationType constellationOfSystemId(int sysId, int* svid) {
    switch (sysId) {
        case 1:
            return GnssConstellationType::GPS;
        case 2:
            if (*svid > 64) *svid -= 64;
            return GnssConstellationType::GLONASS;
        case 3:
            return GnssConstellationType::GALILEO;
        case 4:
            if (*svid > 200) *svid -= 200;
            return GnssConstellationType::BEIDOU;
        case 5:
            return GnssConstellationType::QZSS;
        case 6:
            return GnssConstellationType::IRNSS;
        default:
            return GnssConstellationType::UNKNOWN;
    }
}

// NMEA 4.11 signal ids of the GSV trailer, for the bands the modem tracks
static double carrierOf(GnssConstellationType c, int signalId) {
    switch (c) {
        case GnssConstellationType::GPS:
        case GnssConstellationType::QZSS:
            return signalId >= 7 ? 1176.45e6 : 1575.42e6;
        case GnssConstellationType::GALILEO:
            return signalId == 1 || signalId == 2 ? 1176.45e6 : 1575.42e6;
        case GnssConstellationType::BEIDOU:
            return signalId == 5 ? 1176.45e6 : 1561.098e6;
        case GnssConstellationType::GLONASS:
            return 1602e6;
        case GnssConstellationType::IRNSS:
            return 1176.45e6;
        default:
            return 0;
    }
}

void Gnss::flushSvStatus() {
    if (!mSvs.empty()) {
        for (auto& sv : mSvs) {
            bool used = std::any_of(mUsedSvs.begin(), mUsedSvs.end(), [&sv](const auto& entry) {
                if (entry.second != sv.svid) return false;
                return entry.first == GnssConstellationType::UNKNOWN ||
                       entry.first == sv.constellation;
            });
            if (used) {
                sv.svFlag |= (int)IGnssCallback::GnssSvFlags::USED_IN_FIX;
            }
        }
        if (mIsSvStatusActive) reportSvStatus(filterBlocklistedSatellites(mSvs));
        mSvs.clear();
    }
    mUsedSvs.clear();
}

void Gnss::handlePosition(const std::string& line) {
    // QMI position-report arrives right after GSA for the same 1 Hz epoch.
    flushSvStatus();

    int status = -1;
    unsigned int flags = 0;
    double lat = 0.0, lon = 0.0, alt = 0.0, hacc = 0.0, vacc = 0.0;
    double speed = 0.0, sacc = 0.0, bearing = 0.0, bacc = 0.0;
    uint64_t utcMs = 0;

    int parsed = sscanf(line.c_str(),
                        "pos status=%d flags=%u lat=%lf lon=%lf alt=%lf hacc=%lf vacc=%lf "
                        "speed=%lf sacc=%lf bearing=%lf bacc=%lf utc=%" SCNu64,
                        &status, &flags, &lat, &lon, &alt, &hacc, &vacc, &speed, &sacc, &bearing,
                        &bacc, &utcMs);
    if (parsed != 12) return;

    // status == 0 is QMI_LOC_SESSION_STATUS_SUCCESS (final GNSS fix for this epoch).
    // status == 1 (IN_PROGRESS) carries coarse/intermediate cell-based estimates and must not
    // be reported as a GNSS fix.
    if (status != 0) return;
    if (!(flags & GnssLocation::HAS_LAT_LONG)) return;
    if (!std::isfinite(lat) || !std::isfinite(lon)) return;
    if (lat < -90.0 || lat > 90.0 || lon < -180.0 || lon > 180.0) return;
    if (lat == 0.0 && lon == 0.0) return;

    GnssLocation loc = {};
    int outFlags = GnssLocation::HAS_LAT_LONG;
    loc.latitudeDegrees = lat;
    loc.longitudeDegrees = lon;

    if ((flags & GnssLocation::HAS_ALTITUDE) && std::isfinite(alt)) {
        loc.altitudeMeters = alt;
        outFlags |= GnssLocation::HAS_ALTITUDE;
    }

    if ((flags & GnssLocation::HAS_HORIZONTAL_ACCURACY) && std::isfinite(hacc) && hacc > 0.0) {
        loc.horizontalAccuracyMeters = hacc;
    } else {
        loc.horizontalAccuracyMeters = 50.0;
    }
    outFlags |= GnssLocation::HAS_HORIZONTAL_ACCURACY;

    if ((flags & GnssLocation::HAS_VERTICAL_ACCURACY) && std::isfinite(vacc) && vacc > 0.0) {
        loc.verticalAccuracyMeters = vacc;
        outFlags |= GnssLocation::HAS_VERTICAL_ACCURACY;
    }

    if ((flags & GnssLocation::HAS_SPEED) && std::isfinite(speed) && speed >= 0.0) {
        loc.speedMetersPerSec = speed;
        outFlags |= GnssLocation::HAS_SPEED;
    }

    if ((flags & GnssLocation::HAS_SPEED_ACCURACY) && std::isfinite(sacc) && sacc > 0.0) {
        loc.speedAccuracyMetersPerSecond = sacc;
        outFlags |= GnssLocation::HAS_SPEED_ACCURACY;
    }

    if ((flags & GnssLocation::HAS_BEARING) && std::isfinite(bearing)) {
        bearing = std::fmod(bearing, 360.0);
        if (bearing < 0.0) bearing += 360.0;
        loc.bearingDegrees = bearing;
        outFlags |= GnssLocation::HAS_BEARING;
    }

    if ((flags & GnssLocation::HAS_BEARING_ACCURACY) && std::isfinite(bacc) && bacc > 0.0 &&
        bacc <= 180.0) {
        loc.bearingAccuracyDegrees = bacc;
        outFlags |= GnssLocation::HAS_BEARING_ACCURACY;
    }

    loc.gnssLocationFlags = outFlags;

    if (utcMs > 1577836800000ULL) {
        loc.timestampMillis = static_cast<int64_t>(utcMs);
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        loc.timestampMillis =
                static_cast<int64_t>(ts.tv_sec) * 1000LL + static_cast<int64_t>(ts.tv_nsec) / 1000000LL;
    }

    loc.elapsedRealtime.flags = ElapsedRealtime::HAS_TIMESTAMP_NS |
                                ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
    loc.elapsedRealtime.timestampNs = ::android::elapsedRealtimeNano();
    loc.elapsedRealtime.timeUncertaintyNs = 1000000.0;

    reportLocation(loc);
    mReportedLocationCount += 1;
    mFirstFixReceived = true;
}

void Gnss::handleSentence(const std::string& line) {
    reportNmea(line);
    if (line.size() < 7) return;
    std::string talker = line.substr(1, 2);
    std::string type = line.substr(3, 3);
    std::vector<std::string> f = splitFields(line);

    if (type == "GSA") {
        int sysId = (f.size() >= 19 && !f[18].empty()) ? atoi(f[18].c_str()) : 0;
        for (size_t i = 3; i < f.size() && i < 15; i++) {
            if (f[i].empty()) continue;
            int svid = atoi(f[i].c_str());
            GnssConstellationType c = (sysId > 0) ? constellationOfSystemId(sysId, &svid)
                                                  : constellationOf(talker, &svid);
            mUsedSvs.emplace_back(c, svid);
        }
    } else if (type == "GSV") {
        size_t sats = f.size() > 4 ? (f.size() - 4) / 4 : 0;
        int signalId = (f.size() - 4) % 4 == 1 ? atoi(f.back().c_str()) : 1;
        for (size_t i = 0; i < sats; i++) {
            const std::string& id = f[4 + i * 4];
            if (id.empty()) continue;
            int svid = atoi(id.c_str());
            GnssConstellationType c = constellationOf(talker, &svid);
            if (c == GnssConstellationType::UNKNOWN) c = GnssConstellationType::GPS;
            double carrier = carrierOf(c, signalId);
            IGnssCallback::GnssSvInfo sv = {
                    .svid = svid,
                    .constellation = c,
                    .cN0Dbhz = (float)atof(f[7 + i * 4].c_str()),
                    .basebandCN0DbHz = (float)atof(f[7 + i * 4].c_str()),
                    .elevationDegrees = (float)atof(f[5 + i * 4].c_str()),
                    .azimuthDegrees = (float)atof(f[6 + i * 4].c_str()),
                    .carrierFrequencyHz = (long)carrier,
                    .svFlag = (int)IGnssCallback::GnssSvFlags::NONE,
            };
            if (carrier > 0) sv.svFlag |= (int)IGnssCallback::GnssSvFlags::HAS_CARRIER_FREQUENCY;
            mSvs.push_back(sv);
        }
    }
}
ScopedAStatus Gnss::start() {
    ALOGD("start");
    if (mIsActive) {
        ALOGW("Gnss has started. Restarting...");
        stop();
    }
    GnssPsds::requestPsds(PsdsType::LONG_TERM);
    if (mHelperPid < 0 && !startHelper()) {
        ALOGE("failed to start %s", kHelperPath);
        return ScopedAStatus::fromExceptionCode(STATUS_INVALID_OPERATION);
    }
    mIsActive = true;
    // notify measurement engine to update measurement interval
    if (mGnssMeasurementInterface) {
        mGnssMeasurementInterface->setLocationEnabled(true);
    }
    this->reportGnssStatusValue(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
    mThreadBlocker.notify();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::stop() {
    ALOGD("stop");
    mIsActive = false;
    stopHelper();
    if (mGnssMeasurementInterface) {
        mGnssMeasurementInterface->setLocationEnabled(false);
    }
    this->reportGnssStatusValue(IGnssCallback::GnssStatusValue::SESSION_END);

    int reportedLocationCount = mReportedLocationCount;
    ALOGD("reportedLocationCount: %d", reportedLocationCount);
    mReportedLocationCount = 0;
    mThreadBlocker.notify();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::close() {
    ALOGD("close");
    {
        std::unique_lock<std::mutex> lock(mMutex);
        sGnssCallback = nullptr;
    }
    mIsInitialized = false;
    mThreadBlocker.notify();
    if (mThread.joinable()) {
        mThread.join();
    }
    return ScopedAStatus::ok();
}

void Gnss::reportLocation(const GnssLocation& location) {
    std::shared_ptr<IGnssCallback> callback;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (sGnssCallback == nullptr) {
            ALOGE("%s: GnssCallback is null.", __func__);
            return;
        }
        callback = sGnssCallback;
    }
    mLastLocation = std::make_shared<GnssLocation>(location);
    auto status = callback->gnssLocationCb(location);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke gnssLocationCb", __func__);
    }
    return;
}

// Satellite status comes from the modem's GSV sentences, see handleSentence()
void Gnss::reportSvStatus() const {}

void Gnss::reportSvStatus(const std::vector<GnssSvInfo>& svInfoList) const {
    std::shared_ptr<IGnssCallback> callback;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (sGnssCallback == nullptr) {
            ALOGE("%s: GnssCallback is null.", __func__);
            return;
        }
        callback = sGnssCallback;
    }
    auto status = callback->gnssSvStatusCb(svInfoList);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke callback", __func__);
    }
}

std::vector<GnssSvInfo> Gnss::filterBlocklistedSatellites(
        std::vector<GnssSvInfo> gnssSvInfoList) const {
    if (mGnssConfiguration == nullptr) return gnssSvInfoList;
    for (uint32_t i = 0; i < gnssSvInfoList.size(); i++) {
        if (mGnssConfiguration->isBlocklisted(gnssSvInfoList[i])) {
            gnssSvInfoList[i].svFlag &= ~(uint32_t)IGnssCallback::GnssSvFlags::USED_IN_FIX;
        }
    }
    return gnssSvInfoList;
}

void Gnss::reportGnssStatusValue(const IGnssCallback::GnssStatusValue gnssStatusValue) const {
    std::shared_ptr<IGnssCallback> callback;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        if (sGnssCallback == nullptr) {
            ALOGE("%s: GnssCallback is null.", __func__);
            return;
        }
        callback = sGnssCallback;
    }
    auto status = callback->gnssStatusCb(gnssStatusValue);
    if (!status.isOk()) {
        ALOGE("%s: Unable to invoke gnssStatusCb", __func__);
    }
}

void Gnss::reportNmea(const std::string& sentence) const {
    if (mIsNmeaActive) {
        std::shared_ptr<IGnssCallback> callback;
        {
            std::unique_lock<std::mutex> lock(mMutex);
            if (sGnssCallback == nullptr) {
                ALOGE("%s: GnssCallback is null.", __func__);
                return;
            }
            callback = sGnssCallback;
        }
        nsecs_t now = systemTime(SYSTEM_TIME_MONOTONIC);
        auto status = callback->gnssNmeaCb(now, sentence);
        if (!status.isOk()) {
            ALOGE("%s: Unable to invoke callback", __func__);
        }
    }
}

ScopedAStatus Gnss::startSvStatus() {
    ALOGD("startSvStatus");
    mIsSvStatusActive = true;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::stopSvStatus() {
    ALOGD("stopSvStatus");
    mIsSvStatusActive = false;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::startNmea() {
    ALOGD("startNmea");
    mIsNmeaActive = true;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::stopNmea() {
    ALOGD("stopNmea");
    mIsNmeaActive = false;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionAGnss(std::shared_ptr<IAGnss>* iAGnss) {
    ALOGD("Gnss::getExtensionAGnss");
    *iAGnss = SharedRefBase::make<AGnss>();
    return ndk::ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectTime(int64_t timeMs, int64_t timeReferenceMs, int uncertaintyMs) {
    ALOGD("injectTime. timeMs:%" PRId64 ", timeReferenceMs:%" PRId64 ", uncertaintyMs:%d", timeMs,
          timeReferenceMs, uncertaintyMs);
    if (timeMs > 1577836800000LL && uncertaintyMs >= 0) {
        int64_t adjustedUtcMs = timeMs + (::android::elapsedRealtime() - timeReferenceMs);
        if (adjustedUtcMs > 1577836800000LL) {
            runHelperOneShot({"--inject-time", std::to_string(adjustedUtcMs),
                              std::to_string(uncertaintyMs)});
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionAGnssRil(std::shared_ptr<IAGnssRil>* iAGnssRil) {
    ALOGD("Gnss::getExtensionAGnssRil");
    *iAGnssRil = SharedRefBase::make<AGnssRil>();
    return ndk::ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectLocation(const GnssLocation& location) {
    ALOGD("injectLocation. lat:%lf, lng:%lf, acc:%f", location.latitudeDegrees,
          location.longitudeDegrees, location.horizontalAccuracyMeters);
    if ((location.gnssLocationFlags & GnssLocation::HAS_LAT_LONG) &&
        std::isfinite(location.latitudeDegrees) && std::isfinite(location.longitudeDegrees) &&
        location.latitudeDegrees >= -90.0 && location.latitudeDegrees <= 90.0 &&
        location.longitudeDegrees >= -180.0 && location.longitudeDegrees <= 180.0) {
        double hacc = (std::isfinite(location.horizontalAccuracyMeters) &&
                       location.horizontalAccuracyMeters > 0.0)
                              ? location.horizontalAccuracyMeters
                              : 100.0;
        char latBuf[32], lonBuf[32], accBuf[32];
        snprintf(latBuf, sizeof(latBuf), "%.8f", location.latitudeDegrees);
        snprintf(lonBuf, sizeof(lonBuf), "%.8f", location.longitudeDegrees);
        snprintf(accBuf, sizeof(accBuf), "%.3f", hacc);
        runHelperOneShot({"--inject-pos", latBuf, lonBuf, accBuf});
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectBestLocation(const GnssLocation& location) {
    ALOGD("injectBestLocation. lat:%lf, lng:%lf, acc:%f", location.latitudeDegrees,
          location.longitudeDegrees, location.horizontalAccuracyMeters);
    return injectLocation(location);
}

ScopedAStatus Gnss::deleteAidingData(GnssAidingData aidingDataFlags) {
    ALOGD("deleteAidingData. flags:%d", (int)aidingDataFlags);
    mFirstFixReceived = false;
    GnssPsds::resetInjectionState();
    runHelperOneShot({"--delete-aiding", std::to_string(static_cast<uint32_t>(aidingDataFlags))});
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::setPositionMode(const PositionModeOptions& options) {
    ALOGD("setPositionMode. minIntervalMs:%d, lowPowerMode:%d", options.minIntervalMs,
          (int)options.lowPowerMode);
    if (std::max(1000, options.minIntervalMs) != mMinIntervalMs) {
        mMinIntervalMs = std::max(1000, options.minIntervalMs);
        if (mGnssMeasurementInterface) {
            mGnssMeasurementInterface->setLocationInterval(mMinIntervalMs);
        }
        if (mIsActive) {
            stop();
            start();
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionPsds(std::shared_ptr<IGnssPsds>* iGnssPsds) {
    ALOGD("getExtensionPsds");
    *iGnssPsds = SharedRefBase::make<GnssPsds>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssConfiguration(
        std::shared_ptr<IGnssConfiguration>* iGnssConfiguration) {
    ALOGD("getExtensionGnssConfiguration");
    if (mGnssConfiguration == nullptr) {
        mGnssConfiguration = SharedRefBase::make<GnssConfiguration>();
    }
    *iGnssConfiguration = mGnssConfiguration;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssPowerIndication(
        std::shared_ptr<IGnssPowerIndication>* iGnssPowerIndication) {
    ALOGD("getExtensionGnssPowerIndication");
    if (mGnssPowerIndication == nullptr) {
        mGnssPowerIndication = SharedRefBase::make<GnssPowerIndication>();
    }

    *iGnssPowerIndication = mGnssPowerIndication;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssMeasurement(
        std::shared_ptr<IGnssMeasurementInterface>* iGnssMeasurement) {
    ALOGD("getExtensionGnssMeasurement");
    if (mGnssMeasurementInterface == nullptr) {
        mGnssMeasurementInterface = SharedRefBase::make<GnssMeasurementInterface>();
        mGnssMeasurementInterface->setGnssInterface(static_cast<std::shared_ptr<Gnss>>(this));
    }
    *iGnssMeasurement = mGnssMeasurementInterface;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssBatching(std::shared_ptr<IGnssBatching>* iGnssBatching) {
    ALOGD("getExtensionGnssBatching");

    *iGnssBatching = SharedRefBase::make<GnssBatching>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>* iGnssGeofence) {
    ALOGD("getExtensionGnssGeofence");

    *iGnssGeofence = SharedRefBase::make<GnssGeofence>();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssNavigationMessage(
        std::shared_ptr<IGnssNavigationMessageInterface>* iGnssNavigationMessage) {
    ALOGD("getExtensionGnssNavigationMessage");

    *iGnssNavigationMessage = SharedRefBase::make<GnssNavigationMessageInterface>();
    return ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* iGnssDebug) {
    ALOGD("Gnss::getExtensionGnssDebug");
    *iGnssDebug = SharedRefBase::make<GnssDebug>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssVisibilityControl(
        std::shared_ptr<visibility_control::IGnssVisibilityControl>* iGnssVisibilityControl) {
    ALOGD("Gnss::getExtensionGnssVisibilityControl");

    *iGnssVisibilityControl = SharedRefBase::make<visibility_control::GnssVisibilityControl>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssAntennaInfo(
        std::shared_ptr<IGnssAntennaInfo>* iGnssAntennaInfo) {
    ALOGD("Gnss::getExtensionGnssAntennaInfo");

    *iGnssAntennaInfo = SharedRefBase::make<GnssAntennaInfo>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionMeasurementCorrections(
        std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>*
                iMeasurementCorrections) {
    ALOGD("Gnss::getExtensionMeasurementCorrections");

    *iMeasurementCorrections =
            SharedRefBase::make<measurement_corrections::MeasurementCorrectionsInterface>();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Gnss::getExtensionGnssAssistanceInterface(
        std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>* iGnssAssistanceInterface) {
    ALOGD("Gnss::getExtensionGnssAssistanceInterface");

    *iGnssAssistanceInterface = SharedRefBase::make<gnss_assistance::GnssAssistanceInterface>();
    return ndk::ScopedAStatus::ok();
}

void Gnss::setGnssMeasurementEnabled(const bool enabled) {
    mGnssMeasurementEnabled = enabled;
}

void Gnss::setGnssMeasurementInterval(const long intervalMs) {
    mGnssMeasurementIntervalMs = intervalMs;
}

std::shared_ptr<GnssLocation> Gnss::getLastLocation() const {
    return mLastLocation;
}

}  // namespace aidl::android::hardware::gnss
