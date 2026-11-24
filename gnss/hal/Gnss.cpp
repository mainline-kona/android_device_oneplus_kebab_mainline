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
#include <inttypes.h>
#include <fcntl.h>
#include <log/log.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <set>
#include <sstream>
#include <utils/Timers.h>
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
#include "NmeaFixInfo.h"
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

    int capabilities = (int)IGnssCallback::CAPABILITY_SCHEDULING;
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
    GnssSignalType signalType1 = {
            .constellation = GnssConstellationType::GPS,
            .carrierFrequencyHz = 1.57542e+09,
            .codeType = GnssSignalType::CODE_TYPE_C,
    };
    GnssSignalType signalType2 = {
            .constellation = GnssConstellationType::GLONASS,
            .carrierFrequencyHz = 1.5980625e+09,
            .codeType = GnssSignalType::CODE_TYPE_C,
    };
    status = callback->gnssSetSignalTypeCapabilitiesCb(
            std::vector<GnssSignalType>({signalType1, signalType2}));
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
    pid_t pid = fork();
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        execl(kHelperPath, kHelperPath, (char*)nullptr);
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
    mGga.clear();
    mSvs.clear();
    mUsedSvids.clear();
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
    static std::string pending;
    pending.append(buf, n);
    size_t pos;
    while ((pos = pending.find('\n')) != std::string::npos) {
        std::string line = pending.substr(0, pos);
        pending.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("error=", 0) == 0) ALOGE("kona-gnss: %s", line.c_str());
        if (!line.empty() && line[0] == '$') handleSentence(line);
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
    return GnssConstellationType::GPS;
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
        default:
            return 0;
    }
}

void Gnss::handleSentence(const std::string& line) {
    reportNmea(line);
    if (line.size() < 7) return;
    std::string talker = line.substr(1, 2);
    std::string type = line.substr(3, 3);
    std::vector<std::string> f = splitFields(line);

    if (type == "GGA") {
        // A new epoch starts with GGA: report the satellites gathered for the last one
        if (!mSvs.empty()) {
            for (auto& sv : mSvs) {
                if (std::find(mUsedSvids.begin(), mUsedSvids.end(), sv.svid) != mUsedSvids.end())
                    sv.svFlag |= (int)IGnssCallback::GnssSvFlags::USED_IN_FIX;
            }
            if (mIsSvStatusActive) reportSvStatus(filterBlocklistedSatellites(mSvs));
            mSvs.clear();
        }
        mUsedSvids.clear();
        mGga = f.size() > 6 && !f[6].empty() && f[6] != "0" ? line : "";
    } else if (type == "RMC") {
        // The parser accepts empty sentences as a fix, so only hand it real ones
        if (mGga.empty() || f.size() < 3 || f[2] != "A") return;
        std::string input = "$GPGGA" + mGga.substr(6) + "\n$GPRMC" + line.substr(6) + "\n";
        auto location = ::android::hardware::gnss::common::NmeaFixInfo::getAidlLocationFromInputStr(
                input);
        if (location != nullptr) {
            reportLocation(*location);
            mReportedLocationCount += 1;
        }
    } else if (type == "GSA") {
        for (size_t i = 3; i < f.size() && i < 15; i++) {
            if (!f[i].empty()) mUsedSvids.push_back(atoi(f[i].c_str()));
        }
    } else if (type == "GSV") {
        size_t sats = f.size() > 4 ? (f.size() - 4) / 4 : 0;
        int signalId = (f.size() - 4) % 4 == 1 ? atoi(f.back().c_str()) : 1;
        for (size_t i = 0; i < sats; i++) {
            const std::string& id = f[4 + i * 4];
            if (id.empty()) continue;
            int svid = atoi(id.c_str());
            GnssConstellationType c = constellationOf(talker, &svid);
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
    if (mHelperPid < 0 && !startHelper()) {
        ALOGE("failed to start %s", kHelperPath);
        return ScopedAStatus::fromExceptionCode(STATUS_INVALID_OPERATION);
    }
    mIsActive = true;
    // notify measurement engine to update measurement interval
    mGnssMeasurementInterface->setLocationEnabled(true);
    this->reportGnssStatusValue(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
    mThreadBlocker.notify();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::stop() {
    ALOGD("stop");
    mIsActive = false;
    stopHelper();
    mGnssMeasurementInterface->setLocationEnabled(false);
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
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectBestLocation(const GnssLocation& location) {
    ALOGD("injectBestLocation. lat:%lf, lng:%lf, acc:%f", location.latitudeDegrees,
          location.longitudeDegrees, location.horizontalAccuracyMeters);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::deleteAidingData(GnssAidingData aidingDataFlags) {
    ALOGD("deleteAidingData. flags:%d", (int)aidingDataFlags);
    mFirstFixReceived = false;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::setPositionMode(const PositionModeOptions& options) {
    ALOGD("setPositionMode. minIntervalMs:%d, lowPowerMode:%d", options.minIntervalMs,
          (int)options.lowPowerMode);
    if (std::max(1000, options.minIntervalMs) != mMinIntervalMs) {
        mMinIntervalMs = std::max(1000, options.minIntervalMs);
        mGnssMeasurementInterface->setLocationInterval(mMinIntervalMs);
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
