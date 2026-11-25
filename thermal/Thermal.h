//
// Copyright (C) 2026 The PenguinOS Project
// SPDX-License-Identifier: Apache-2.0
//

#pragma once

#include <aidl/android/hardware/thermal/BnThermal.h>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace aidl::android::hardware::thermal::impl::kona {

inline constexpr size_t kSeverityCount = 7;

struct SensorEntry {
    int zoneIndex = -1;
    std::string sysfsTempPath;
    std::string name;
    TemperatureType type = TemperatureType::UNKNOWN;
    std::array<float, kSeverityCount> hotThresholds{};
    std::array<float, kSeverityCount> coldThresholds{};
    ThrottlingSeverity lastSeverity = ThrottlingSeverity::NONE;
};

struct CoolingEntry {
    int cdevIndex = -1;
    std::string sysfsCurStatePath;
    std::string name;
    CoolingType type = CoolingType::COMPONENT;
    int64_t lastState = 0;
};

class Thermal : public BnThermal {
  public:
    Thermal();
    ~Thermal() override;

    ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out_devices) override;
    ndk::ScopedAStatus getCoolingDevicesWithType(CoolingType in_type,
                                                 std::vector<CoolingDevice>* out_devices) override;

    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out_temperatures) override;
    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType in_type,
                                               std::vector<Temperature>* out_temperatures) override;

    ndk::ScopedAStatus getTemperatureThresholds(
            std::vector<TemperatureThreshold>* out_temperatureThresholds) override;
    ndk::ScopedAStatus getTemperatureThresholdsWithType(
            TemperatureType in_type,
            std::vector<TemperatureThreshold>* out_temperatureThresholds) override;

    ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& in_callback) override;
    ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& in_callback,
            TemperatureType in_type) override;
    ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& in_callback) override;

    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback,
            CoolingType in_type) override;
    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& in_callback) override;

    ndk::ScopedAStatus forecastSkinTemperature(int32_t forecastSeconds,
                                               float* _aidl_return) override;

  private:
    struct ThermalCallbackRecord {
        std::shared_ptr<IThermalChangedCallback> callback;
        std::optional<TemperatureType> filter;
    };

    struct CoolingCallbackRecord {
        std::shared_ptr<ICoolingDeviceChangedCallback> callback;
        CoolingType filter;
    };

    void scanThermalZones();
    void scanCoolingDevices();
    void monitorLoop();
    Temperature readSensorLocked(SensorEntry& entry) const;
    CoolingDevice readCoolingLocked(const CoolingEntry& entry) const;

    mutable std::mutex state_mutex_;
    std::vector<SensorEntry> sensors_;
    std::vector<CoolingEntry> cooling_devices_;

    std::mutex callback_mutex_;
    std::condition_variable monitor_cv_;
    bool stop_monitor_ = false;
    std::vector<ThermalCallbackRecord> thermal_callbacks_;
    std::vector<CoolingCallbackRecord> cdev_callbacks_;
    std::thread monitor_thread_;
};

}  // namespace aidl::android::hardware::thermal::impl::kona
