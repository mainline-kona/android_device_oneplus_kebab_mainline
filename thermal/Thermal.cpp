//
// Copyright (C) 2026 The PenguinOS Project
// SPDX-License-Identifier: Apache-2.0
//

#define LOG_TAG "ThermalHalKona"

#include "Thermal.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>

namespace aidl::android::hardware::thermal::impl::kona {

using ndk::ScopedAStatus;

namespace {

constexpr char kThermalSysfsDir[] = "/sys/class/thermal";
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();
constexpr float kHysteresisC = 1.0f;
constexpr auto kMonitorInterval = std::chrono::milliseconds(2000);

bool interfacesEqual(const std::shared_ptr<::ndk::ICInterface>& left,
                     const std::shared_ptr<::ndk::ICInterface>& right) {
    if (left == nullptr || right == nullptr || !left->isRemote() || !right->isRemote()) {
        return left == right;
    }
    return left->asBinder() == right->asBinder();
}

std::array<float, kSeverityCount> makeNanArray() {
    return {kNaN, kNaN, kNaN, kNaN, kNaN, kNaN, kNaN};
}

std::array<float, kSeverityCount> makeHotThresholds(float light, float moderate, float severe,
                                                    float critical, float emergency,
                                                    float shutdown) {
    return {kNaN, light, moderate, severe, critical, emergency, shutdown};
}

bool readTrimmed(const std::string& path, std::string* out) {
    std::string raw;
    if (!::android::base::ReadFileToString(path, &raw)) {
        return false;
    }
    *out = ::android::base::Trim(raw);
    return !out->empty();
}

bool parseSysfsIndex(const std::string& name, const std::string& prefix, int* index) {
    if (name.rfind(prefix, 0) != 0 || name.size() == prefix.size()) {
        return false;
    }
    const char* start = name.c_str() + prefix.size();
    char* end = nullptr;
    long val = std::strtol(start, &end, 10);
    if (end == start || *end != '\0' || val < 0 || val > 10000) {
        return false;
    }
    *index = static_cast<int>(val);
    return true;
}

bool readTempCelsius(const std::string& path, float* outC) {
    std::string raw;
    if (!readTrimmed(path, &raw)) {
        return false;
    }
    char* end = nullptr;
    long milliC = std::strtol(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        return false;
    }
    *outC = static_cast<float>(milliC) / 1000.0f;
    return std::isfinite(*outC);
}

bool readInt64(const std::string& path, int64_t* out) {
    std::string raw;
    if (!readTrimmed(path, &raw)) {
        return false;
    }
    char* end = nullptr;
    long long val = std::strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0') {
        return false;
    }
    *out = static_cast<int64_t>(val);
    return true;
}

ThrottlingSeverity computeSeverity(float tempC, const std::array<float, kSeverityCount>& hot,
                                   ThrottlingSeverity prev) {
    if (!std::isfinite(tempC)) {
        return ThrottlingSeverity::NONE;
    }
    int prevIdx = static_cast<int>(prev);
    for (int i = static_cast<int>(kSeverityCount) - 1; i >= 1; --i) {
        float thresh = hot[static_cast<size_t>(i)];
        if (!std::isfinite(thresh)) continue;
        float effective = (i <= prevIdx) ? (thresh - kHysteresisC) : thresh;
        if (tempC >= effective) {
            return static_cast<ThrottlingSeverity>(i);
        }
    }
    return ThrottlingSeverity::NONE;
}

void classifyZone(const std::string& zoneType, TemperatureType* outType,
                  std::array<float, kSeverityCount>* outHot) {
    *outHot = makeNanArray();

    if (zoneType.rfind("cpu", 0) == 0 || zoneType.rfind("cluster", 0) == 0) {
        *outType = TemperatureType::CPU;
        *outHot = makeHotThresholds(75.0f, 85.0f, 90.0f, 95.0f, 105.0f, 110.0f);
        return;
    }
    if (zoneType.rfind("gpu", 0) == 0) {
        *outType = TemperatureType::GPU;
        *outHot = makeHotThresholds(75.0f, 80.0f, 85.0f, 90.0f, 105.0f, 110.0f);
        return;
    }
    if (zoneType.rfind("bq28z", 0) == 0 || zoneType.find("batt") != std::string::npos) {
        *outType = TemperatureType::BATTERY;
        *outHot = makeHotThresholds(40.0f, 43.0f, 45.0f, 48.0f, 52.0f, 60.0f);
        return;
    }
    if (zoneType == "skin-thermal") {
        *outType = TemperatureType::SKIN;
        *outHot = makeHotThresholds(39.0f, 43.0f, 46.0f, 48.0f, 52.0f, 60.0f);
        return;
    }
    if (zoneType == "skin-pm8150-thermal") {
        *outType = TemperatureType::SKIN;
        *outHot = makeHotThresholds(43.0f, 47.0f, 51.0f, 55.0f, 60.0f, 70.0f);
        return;
    }
    if (zoneType == "skin-msm-thermal") {
        *outType = TemperatureType::SKIN;
        *outHot = makeHotThresholds(48.0f, 54.0f, 60.0f, 63.0f, 65.0f, 75.0f);
        return;
    }
    if (zoneType == "conn-thermal" || zoneType.find("usb") != std::string::npos) {
        *outType = TemperatureType::USB_PORT;
        *outHot = makeHotThresholds(45.0f, 50.0f, 55.0f, 60.0f, 65.0f, 75.0f);
        return;
    }
    if (zoneType.rfind("pa", 0) == 0 && zoneType.find("therm") != std::string::npos) {
        *outType = TemperatureType::POWER_AMPLIFIER;
        *outHot = makeHotThresholds(kNaN, kNaN, 65.0f, 75.0f, 85.0f, 95.0f);
        return;
    }
    if (zoneType == "npu-thermal" || zoneType == "compute-thermal" ||
        zoneType == "q6-hvx-thermal") {
        *outType = TemperatureType::NPU;
        *outHot = makeHotThresholds(kNaN, kNaN, 90.0f, 95.0f, 105.0f, 110.0f);
        return;
    }
    if (zoneType == "wlan-thermal" || zoneType.find("wifi") != std::string::npos) {
        *outType = TemperatureType::WIFI;
        *outHot = makeHotThresholds(kNaN, kNaN, 90.0f, 95.0f, 105.0f, 110.0f);
        return;
    }
    if (zoneType == "cam-flash-thermal") {
        *outType = TemperatureType::FLASHLIGHT;
        *outHot = makeHotThresholds(kNaN, kNaN, 65.0f, 75.0f, 85.0f, 95.0f);
        return;
    }
    if (zoneType == "camera-thermal") {
        *outType = TemperatureType::CAMERA;
        *outHot = makeHotThresholds(kNaN, kNaN, 90.0f, 95.0f, 105.0f, 110.0f);
        return;
    }
    if (zoneType.rfind("pm8150", 0) == 0) {
        *outType = TemperatureType::SOC;
        *outHot = makeHotThresholds(kNaN, kNaN, 95.0f, 115.0f, 130.0f, 145.0f);
        return;
    }
    *outType = TemperatureType::SOC;
    *outHot = makeHotThresholds(kNaN, kNaN, 90.0f, 95.0f, 105.0f, 110.0f);
}

CoolingType classifyCoolingDevice(const std::string& typeName) {
    if (typeName.rfind("cpufreq", 0) == 0 || typeName.rfind("cpuidle", 0) == 0 ||
        typeName.find("cpu") != std::string::npos) {
        return CoolingType::CPU;
    }
    if (typeName.find("gpu") != std::string::npos) {
        return CoolingType::GPU;
    }
    if (typeName.find("ath11k") != std::string::npos ||
        typeName.find("wlan") != std::string::npos || typeName.find("wifi") != std::string::npos) {
        return CoolingType::WIFI;
    }
    if (typeName.find("batt") != std::string::npos || typeName.find("chg") != std::string::npos) {
        return CoolingType::BATTERY;
    }
    if (typeName.find("modem") != std::string::npos) {
        return CoolingType::MODEM;
    }
    return CoolingType::COMPONENT;
}

}  // namespace

Thermal::Thermal() {
    scanThermalZones();
    scanCoolingDevices();
    LOG(INFO) << "Initialized Kona mainline thermal HAL: " << sensors_.size() << " sensors, "
              << cooling_devices_.size() << " cooling devices";
    monitor_thread_ = std::thread(&Thermal::monitorLoop, this);
}

Thermal::~Thermal() {
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        stop_monitor_ = true;
    }
    monitor_cv_.notify_all();
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void Thermal::scanThermalZones() {
    DIR* dir = opendir(kThermalSysfsDir);
    if (!dir) {
        PLOG(ERROR) << "Failed to open " << kThermalSysfsDir;
        return;
    }

    std::vector<std::pair<int, std::string>> zones;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        int idx = -1;
        if (parseSysfsIndex(ent->d_name, "thermal_zone", &idx)) {
            zones.emplace_back(idx, std::string(kThermalSysfsDir) + "/" + ent->d_name);
        }
    }
    closedir(dir);

    std::sort(zones.begin(), zones.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::set<std::pair<TemperatureType, std::string>> seen;
    bool hasSkin = false;

    for (const auto& [idx, path] : zones) {
        std::string zoneType;
        if (!readTrimmed(path + "/type", &zoneType)) {
            continue;
        }
        SensorEntry entry;
        entry.zoneIndex = idx;
        entry.sysfsTempPath = path + "/temp";
        entry.name = zoneType;
        entry.coldThresholds = makeNanArray();
        classifyZone(zoneType, &entry.type, &entry.hotThresholds);

        if (!seen.emplace(entry.type, entry.name).second) {
            entry.name = zoneType + "_" + std::to_string(idx);
            seen.emplace(entry.type, entry.name);
        }
        if (entry.type == TemperatureType::SKIN) {
            hasSkin = true;
        }

        float tempC = kNaN;
        if (readTempCelsius(entry.sysfsTempPath, &tempC)) {
            entry.lastSeverity = computeSeverity(tempC, entry.hotThresholds, ThrottlingSeverity::NONE);
        }
        sensors_.push_back(std::move(entry));
    }

    // If booting an older DTB without PMIC ADC-TM skin thermistors, promote pm8150b-thermal
    // as a SKIN fallback so Android ThermalManagerService headroom & display clamper still work.
    if (!hasSkin) {
        for (auto& s : sensors_) {
            if (s.name == "pm8150b-thermal") {
                s.type = TemperatureType::SKIN;
                s.hotThresholds = makeHotThresholds(45.0f, 50.0f, 55.0f, 60.0f, 70.0f, 85.0f);
                float tempC = kNaN;
                if (readTempCelsius(s.sysfsTempPath, &tempC)) {
                    s.lastSeverity =
                            computeSeverity(tempC, s.hotThresholds, ThrottlingSeverity::NONE);
                }
                LOG(INFO) << "No dedicated SKIN thermistor found; using pm8150b-thermal fallback";
                break;
            }
        }
    }
}

void Thermal::scanCoolingDevices() {
    DIR* dir = opendir(kThermalSysfsDir);
    if (!dir) {
        return;
    }

    std::vector<std::pair<int, std::string>> cdevs;
    struct dirent* ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        int idx = -1;
        if (parseSysfsIndex(ent->d_name, "cooling_device", &idx)) {
            cdevs.emplace_back(idx, std::string(kThermalSysfsDir) + "/" + ent->d_name);
        }
    }
    closedir(dir);

    std::sort(cdevs.begin(), cdevs.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::set<std::pair<CoolingType, std::string>> seen;
    for (const auto& [idx, path] : cdevs) {
        std::string cdevType;
        if (!readTrimmed(path + "/type", &cdevType)) {
            continue;
        }
        CoolingEntry entry;
        entry.cdevIndex = idx;
        entry.sysfsCurStatePath = path + "/cur_state";
        entry.name = cdevType;
        entry.type = classifyCoolingDevice(cdevType);

        if (!seen.emplace(entry.type, entry.name).second) {
            entry.name = cdevType + "_" + std::to_string(idx);
            seen.emplace(entry.type, entry.name);
        }
        int64_t state = 0;
        if (readInt64(entry.sysfsCurStatePath, &state) && state >= 0) {
            entry.lastState = state;
        }
        cooling_devices_.push_back(std::move(entry));
    }
}

Temperature Thermal::readSensorLocked(SensorEntry& entry) const {
    Temperature t;
    t.type = entry.type;
    t.name = entry.name;
    float tempC = kNaN;
    if (readTempCelsius(entry.sysfsTempPath, &tempC)) {
        t.value = tempC;
        t.throttlingStatus = computeSeverity(tempC, entry.hotThresholds, entry.lastSeverity);
        entry.lastSeverity = t.throttlingStatus;
    } else {
        t.value = kNaN;
        t.throttlingStatus = ThrottlingSeverity::NONE;
    }
    return t;
}

CoolingDevice Thermal::readCoolingLocked(const CoolingEntry& entry) const {
    CoolingDevice c;
    c.type = entry.type;
    c.name = entry.name;
    int64_t state = 0;
    if (readInt64(entry.sysfsCurStatePath, &state) && state >= 0) {
        c.value = state;
    } else {
        c.value = entry.lastState;
    }
    c.powerLimitMw = 0;
    c.powerMw = 0;
    c.timeWindowMs = 0;
    return c;
}

void Thermal::monitorLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(callback_mutex_);
            monitor_cv_.wait(lock, [this]() {
                return stop_monitor_ || !thermal_callbacks_.empty() || !cdev_callbacks_.empty();
            });
            if (stop_monitor_) return;
            monitor_cv_.wait_for(lock, kMonitorInterval, [this]() { return stop_monitor_; });
            if (stop_monitor_) return;
        }

        std::vector<Temperature> changedTemps;
        std::vector<CoolingDevice> changedCdevs;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (auto& s : sensors_) {
                ThrottlingSeverity prev = s.lastSeverity;
                Temperature cur = readSensorLocked(s);
                if (cur.throttlingStatus != prev) {
                    changedTemps.push_back(std::move(cur));
                }
            }
            for (auto& c : cooling_devices_) {
                CoolingDevice cur = readCoolingLocked(c);
                if (cur.value != c.lastState) {
                    c.lastState = cur.value;
                    changedCdevs.push_back(std::move(cur));
                }
            }
        }

        if (!changedTemps.empty()) {
            std::vector<ThermalCallbackRecord> cbs;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cbs = thermal_callbacks_;
            }
            std::vector<std::shared_ptr<IThermalChangedCallback>> dead;
            for (const auto& temp : changedTemps) {
                for (const auto& rec : cbs) {
                    if (rec.filter.has_value() && rec.filter.value() != temp.type) continue;
                    auto status = rec.callback->notifyThrottling(temp);
                    if (!status.isOk() && status.getStatus() == STATUS_DEAD_OBJECT) {
                        dead.push_back(rec.callback);
                    }
                }
            }
            if (!dead.empty()) {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                thermal_callbacks_.erase(
                        std::remove_if(thermal_callbacks_.begin(), thermal_callbacks_.end(),
                                       [&](const ThermalCallbackRecord& rec) {
                                           return std::any_of(dead.begin(), dead.end(),
                                                              [&](const auto& d) {
                                                                  return interfacesEqual(
                                                                          rec.callback, d);
                                                              });
                                       }),
                        thermal_callbacks_.end());
            }
        }

        if (!changedCdevs.empty()) {
            std::vector<CoolingCallbackRecord> cbs;
            {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cbs = cdev_callbacks_;
            }
            std::vector<std::shared_ptr<ICoolingDeviceChangedCallback>> dead;
            for (const auto& dev : changedCdevs) {
                for (const auto& rec : cbs) {
                    if (rec.filter != dev.type) continue;
                    auto status = rec.callback->notifyCoolingDeviceChanged(dev);
                    if (!status.isOk() && status.getStatus() == STATUS_DEAD_OBJECT) {
                        dead.push_back(rec.callback);
                    }
                }
            }
            if (!dead.empty()) {
                std::lock_guard<std::mutex> lock(callback_mutex_);
                cdev_callbacks_.erase(
                        std::remove_if(cdev_callbacks_.begin(), cdev_callbacks_.end(),
                                       [&](const CoolingCallbackRecord& rec) {
                                           return std::any_of(dead.begin(), dead.end(),
                                                              [&](const auto& d) {
                                                                  return interfacesEqual(
                                                                          rec.callback, d);
                                                              });
                                       }),
                        cdev_callbacks_.end());
            }
        }
    }
}

ScopedAStatus Thermal::getCoolingDevices(std::vector<CoolingDevice>* out_devices) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_devices->clear();
    out_devices->reserve(cooling_devices_.size());
    for (const auto& c : cooling_devices_) {
        out_devices->push_back(readCoolingLocked(c));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getCoolingDevicesWithType(CoolingType in_type,
                                                 std::vector<CoolingDevice>* out_devices) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_devices->clear();
    for (const auto& c : cooling_devices_) {
        if (c.type == in_type) {
            out_devices->push_back(readCoolingLocked(c));
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatures(std::vector<Temperature>* out_temperatures) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_temperatures->clear();
    out_temperatures->reserve(sensors_.size());
    for (auto& s : sensors_) {
        out_temperatures->push_back(readSensorLocked(s));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperaturesWithType(TemperatureType in_type,
                                               std::vector<Temperature>* out_temperatures) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_temperatures->clear();
    for (auto& s : sensors_) {
        if (s.type == in_type) {
            out_temperatures->push_back(readSensorLocked(s));
        }
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholds(
        std::vector<TemperatureThreshold>* out_temperatureThresholds) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_temperatureThresholds->clear();
    out_temperatureThresholds->reserve(sensors_.size());
    for (const auto& s : sensors_) {
        TemperatureThreshold th;
        th.type = s.type;
        th.name = s.name;
        th.hotThrottlingThresholds.assign(s.hotThresholds.begin(), s.hotThresholds.end());
        th.coldThrottlingThresholds.assign(s.coldThresholds.begin(), s.coldThresholds.end());
        out_temperatureThresholds->push_back(std::move(th));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholdsWithType(
        TemperatureType in_type, std::vector<TemperatureThreshold>* out_temperatureThresholds) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    out_temperatureThresholds->clear();
    for (const auto& s : sensors_) {
        if (s.type != in_type) continue;
        TemperatureThreshold th;
        th.type = s.type;
        th.name = s.name;
        th.hotThrottlingThresholds.assign(s.hotThresholds.begin(), s.hotThresholds.end());
        th.coldThrottlingThresholds.assign(s.coldThresholds.begin(), s.coldThresholds.end());
        out_temperatureThresholds->push_back(std::move(th));
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& in_callback) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (std::any_of(thermal_callbacks_.begin(), thermal_callbacks_.end(),
                        [&](const ThermalCallbackRecord& rec) {
                            return interfacesEqual(rec.callback, in_callback);
                        })) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                               "Callback already registered");
        }
        thermal_callbacks_.push_back({in_callback, std::nullopt});
    }
    monitor_cv_.notify_one();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerThermalChangedCallbackWithType(
        const std::shared_ptr<IThermalChangedCallback>& in_callback, TemperatureType in_type) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (std::any_of(thermal_callbacks_.begin(), thermal_callbacks_.end(),
                        [&](const ThermalCallbackRecord& rec) {
                            return interfacesEqual(rec.callback, in_callback);
                        })) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                               "Callback already registered");
        }
        thermal_callbacks_.push_back({in_callback, in_type});
    }
    monitor_cv_.notify_one();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::unregisterThermalChangedCallback(
        const std::shared_ptr<IThermalChangedCallback>& in_callback) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    bool removed = false;
    thermal_callbacks_.erase(
            std::remove_if(thermal_callbacks_.begin(), thermal_callbacks_.end(),
                           [&](const ThermalCallbackRecord& rec) {
                               if (interfacesEqual(rec.callback, in_callback)) {
                                   removed = true;
                                   return true;
                               }
                               return false;
                           }),
            thermal_callbacks_.end());
    if (!removed) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Callback wasn't registered");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerCoolingDeviceChangedCallbackWithType(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback, CoolingType in_type) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        if (std::any_of(cdev_callbacks_.begin(), cdev_callbacks_.end(),
                        [&](const CoolingCallbackRecord& rec) {
                            return interfacesEqual(rec.callback, in_callback);
                        })) {
            return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                               "Callback already registered");
        }
        cdev_callbacks_.push_back({in_callback, in_type});
    }
    monitor_cv_.notify_one();
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::unregisterCoolingDeviceChangedCallback(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback) {
    if (in_callback == nullptr) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Invalid nullptr callback");
    }
    std::lock_guard<std::mutex> lock(callback_mutex_);
    bool removed = false;
    cdev_callbacks_.erase(
            std::remove_if(cdev_callbacks_.begin(), cdev_callbacks_.end(),
                           [&](const CoolingCallbackRecord& rec) {
                               if (interfacesEqual(rec.callback, in_callback)) {
                                   removed = true;
                                   return true;
                               }
                               return false;
                           }),
            cdev_callbacks_.end());
    if (!removed) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "Callback wasn't registered");
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::forecastSkinTemperature(int32_t forecastSeconds, float* _aidl_return) {
    if (forecastSeconds < 0) {
        return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT,
                                                           "forecastSeconds must be non-negative");
    }
    *_aidl_return = kNaN;
    return ScopedAStatus::fromExceptionCodeWithMessage(
            EX_UNSUPPORTED_OPERATION, "Skin temperature forecasting is not supported");
}

}  // namespace aidl::android::hardware::thermal::impl::kona
