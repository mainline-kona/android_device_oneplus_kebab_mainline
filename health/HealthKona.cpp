//
// Copyright (C) 2026 The PenguinOS Project
// SPDX-License-Identifier: Apache-2.0
//

#define LOG_TAG "HealthHalKona"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_interface_utils.h>
#include <health-impl/Health.h>
#include <health/utils.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

#ifndef CHARGER_FORCE_NO_UI
#define CHARGER_FORCE_NO_UI 0
#endif

#if !CHARGER_FORCE_NO_UI
#include <health-impl/ChargerUtils.h>
#endif

namespace aidl::android::hardware::health::impl::kona {

using ndk::ScopedAStatus;

namespace {

constexpr char kBq28zChargeCounterPath[] = "/sys/class/power_supply/bq28z610-0/charge_counter";
constexpr char kBq28zChargeNowPath[] = "/sys/class/power_supply/bq28z610-0/charge_now";
constexpr char kBq28zVoltageNowPath[] = "/sys/class/power_supply/bq28z610-0/voltage_now";
constexpr char kMp2650ChargeBehaviourPath[] =
        "/sys/class/power_supply/mp2650-charger/charge_behaviour";
constexpr char kUfsHealthDir[] = "/sys/devices/platform/soc@0/1d84000.ufshc/health_descriptor";
constexpr char kUfsStringsDir[] = "/sys/devices/platform/soc@0/1d84000.ufshc/string_descriptors";
constexpr char kUfsDiskStatPath[] = "/sys/block/sda/stat";

constexpr int32_t kDualCellVoltageThresholdMv = 5000;
constexpr int32_t kLongLifeChargeLimitPct = 80;

bool readTrimmed(const std::string& path, std::string* out) {
    std::string raw;
    if (!::android::base::ReadFileToString(path, &raw)) {
        return false;
    }
    // UFS string_descriptors may contain embedded NUL bytes
    raw.erase(std::remove(raw.begin(), raw.end(), '\0'), raw.end());
    *out = ::android::base::Trim(raw);
    return !out->empty();
}

bool readInt32(const std::string& path, int32_t* out, int base = 0) {
    std::string s;
    if (!readTrimmed(path, &s)) return false;
    char* end = nullptr;
    long val = std::strtol(s.c_str(), &end, base);
    if (end == s.c_str() || *end != '\0') return false;
    *out = static_cast<int32_t>(val);
    return true;
}

bool isDualCellPack() {
    int32_t uv = 0;
    return readInt32(kBq28zVoltageNowPath, &uv, 10) && uv > kDualCellVoltageThresholdMv * 1000;
}

bool readRawChargeCounterUah(int32_t* out) {
    if (readInt32(kBq28zChargeCounterPath, out, 10) && *out > 0) {
        return true;
    }
    if (readInt32(kBq28zChargeNowPath, out, 10) && *out > 0) {
        return true;
    }
    return false;
}

void populateStorageInfo(std::vector<StorageInfo>* out) {
    out->clear();
    int32_t eol = 0, lifeA = 0, lifeB = 0;
    if (!readInt32(std::string(kUfsHealthDir) + "/eol_info", &eol, 0) &&
        !readInt32(std::string(kUfsHealthDir) + "/life_time_estimation_a", &lifeA, 0) &&
        !readInt32(std::string(kUfsHealthDir) + "/life_time_estimation_b", &lifeB, 0)) {
        return;
    }
    readInt32(std::string(kUfsHealthDir) + "/life_time_estimation_a", &lifeA, 0);
    readInt32(std::string(kUfsHealthDir) + "/life_time_estimation_b", &lifeB, 0);

    StorageInfo info;
    info.eol = eol;
    info.lifetimeA = lifeA;
    info.lifetimeB = lifeB;

    std::string mfr, prod, rev;
    readTrimmed(std::string(kUfsStringsDir) + "/manufacturer_name", &mfr);
    readTrimmed(std::string(kUfsStringsDir) + "/product_name", &prod);
    readTrimmed(std::string(kUfsStringsDir) + "/product_revision", &rev);
    std::string ver;
    if (!mfr.empty()) ver += mfr;
    if (!prod.empty()) {
        if (!ver.empty()) ver += " ";
        ver += prod;
    }
    if (!rev.empty()) {
        if (!ver.empty()) ver += " ";
        ver += rev;
    }
    info.version = ver;
    out->push_back(std::move(info));
}

void populateDiskStats(std::vector<DiskStats>* out) {
    out->clear();
    std::string content;
    if (!readTrimmed(kUfsDiskStatPath, &content)) {
        return;
    }
    DiskStats stats;
    int parsed = std::sscanf(content.c_str(),
                             "%" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64
                             " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64 " %" SCNd64
                             " %" SCNd64,
                             &stats.reads, &stats.readMerges, &stats.readSectors, &stats.readTicks,
                             &stats.writes, &stats.writeMerges, &stats.writeSectors,
                             &stats.writeTicks, &stats.ioInFlight, &stats.ioTicks,
                             &stats.ioInQueue);
    if (parsed == 11) {
        out->push_back(stats);
    }
}

}  // namespace

class HealthKona : public Health {
  public:
    HealthKona(std::string_view instance_name, std::unique_ptr<struct healthd_config>&& config)
        : Health(instance_name, std::move(config)) {}

    ScopedAStatus getChargeCounterUah(int32_t* out) override {
        int32_t raw = 0;
        if (!readRawChargeCounterUah(&raw)) {
            return Health::getChargeCounterUah(out);
        }
        *out = isDualCellPack() ? (raw * 2) : raw;
        return ScopedAStatus::ok();
    }

    ScopedAStatus getCurrentNowMicroamps(int32_t* out) override {
        auto status = Health::getCurrentNowMicroamps(out);
        if (status.isOk() && isDualCellPack()) {
            *out *= 2;
        }
        return status;
    }

    ScopedAStatus getStorageInfo(std::vector<StorageInfo>* out) override {
        populateStorageInfo(out);
        return ScopedAStatus::ok();
    }

    ScopedAStatus getDiskStats(std::vector<DiskStats>* out) override {
        populateDiskStats(out);
        return ScopedAStatus::ok();
    }

    ScopedAStatus getChargingPolicy(BatteryChargingPolicy* out) override {
        std::lock_guard<std::mutex> lock(policy_mutex_);
        *out = charging_policy_;
        return ScopedAStatus::ok();
    }

    ScopedAStatus setChargingPolicy(BatteryChargingPolicy in_value) override {
        if (in_value != BatteryChargingPolicy::DEFAULT &&
            in_value != BatteryChargingPolicy::LONG_LIFE &&
            in_value != BatteryChargingPolicy::FORCE_FULL_CHARGE) {
            return ScopedAStatus::fromExceptionCodeWithMessage(
                    EX_UNSUPPORTED_OPERATION, "Unsupported charging policy");
        }
        {
            std::lock_guard<std::mutex> lock(policy_mutex_);
            charging_policy_ = in_value;
            applyChargingPolicyLocked(last_level_);
        }
        return ScopedAStatus::ok();
    }

    ScopedAStatus getBatteryHealthData(BatteryHealthData* out) override {
        auto status = Health::getBatteryHealthData(out);
        if (!status.isOk()) {
            *out = BatteryHealthData{};
        }
        int32_t full = getFullChargeUah();
        int32_t design = getFullChargeDesignCapacityUah();
        if (full > 0 && design > 0) {
            int64_t soh = (static_cast<int64_t>(full) * 100 + design / 2) / design;
            out->batteryStateOfHealth = std::clamp<int64_t>(soh, 1, 100);
        }
        if (!out->batteryManufacturer.has_value() || out->batteryManufacturer->empty()) {
            out->batteryManufacturer = "Texas Instruments";
        }
        out->batteryModelName = "BLP801";
        out->batteryVoltageMinDesignUv = 3400000;
        return ScopedAStatus::ok();
    }

  protected:
    void UpdateHealthInfo(HealthInfo* health_info) override {
        // 1. Ensure charge counter is populated even on kernels exposing only charge_now
        if (health_info->batteryChargeCounterUah <= 0) {
            int32_t raw = 0;
            if (readRawChargeCounterUah(&raw)) {
                health_info->batteryChargeCounterUah = raw;
            }
        }

        // 2. Normalize 2S dual-cell (OnePlus 8T 2x 2250 mAh series pack) to 1S-equivalent
        //    values (~4.3 V, ~4440 mAh design) so Android BatteryService, power_profile.xml
        //    (4500 mAh), and apps see consistent single-cell voltage and capacity while
        //    preserving exact wattage (V/2 * 2*I = V*I).
        if (health_info->batteryVoltageMillivolts > kDualCellVoltageThresholdMv) {
            health_info->batteryVoltageMillivolts /= 2;
            health_info->batteryFullChargeUah *= 2;
            health_info->batteryFullChargeDesignCapacityUah *= 2;
            health_info->batteryChargeCounterUah *= 2;
            health_info->batteryCurrentMicroamps *= 2;
            health_info->batteryCurrentAverageMicroamps *= 2;
        }

        // 3. Classify Type-C 3A / USB-PD sources as AC (fast/wall) instead of slow USB
        if (health_info->chargerUsbOnline &&
            (health_info->maxChargingVoltageMicrovolts > 5000000 ||
             health_info->maxChargingCurrentMicroamps >= 1500000)) {
            health_info->chargerAcOnline = true;
            health_info->chargerUsbOnline = false;
        }

        // 4. Populate BatteryHealthData (State of Health from full / design capacity)
        if (!health_info->batteryHealthData.has_value()) {
            health_info->batteryHealthData = BatteryHealthData{};
        }
        auto& bhd = health_info->batteryHealthData.value();
        if (health_info->batteryFullChargeUah > 0 &&
            health_info->batteryFullChargeDesignCapacityUah > 0) {
            int64_t soh = (static_cast<int64_t>(health_info->batteryFullChargeUah) * 100 +
                           health_info->batteryFullChargeDesignCapacityUah / 2) /
                          health_info->batteryFullChargeDesignCapacityUah;
            bhd.batteryStateOfHealth = std::clamp<int64_t>(soh, 1, 100);
        }
        if (!bhd.batteryManufacturer.has_value() || bhd.batteryManufacturer->empty()) {
            bhd.batteryManufacturer = "Texas Instruments";
        }
        bhd.batteryModelName = "BLP801";
        bhd.batteryVoltageMinDesignUv = 3400000;

        // 5. Populate UFS storage health & disk stats
        populateStorageInfo(&health_info->storageInfos);
        populateDiskStats(&health_info->diskStats);

        // 6. Enforce & report chargingPolicy / chargingState
        BatteryChargingPolicy policy;
        {
            std::lock_guard<std::mutex> lock(policy_mutex_);
            last_level_ = health_info->batteryLevel;
            policy = charging_policy_;
            applyChargingPolicyLocked(last_level_);
        }
        health_info->chargingPolicy = policy;

        bool plugged = health_info->chargerAcOnline || health_info->chargerUsbOnline ||
                       health_info->chargerWirelessOnline || health_info->chargerDockOnline;
        if (!plugged) {
            health_info->chargingState = BatteryChargingState::INVALID;
        } else if (health_info->batteryTemperatureTenthsCelsius >= 450) {
            health_info->chargingState = BatteryChargingState::TOO_HOT;
        } else if (health_info->batteryTemperatureTenthsCelsius <= 0) {
            health_info->chargingState = BatteryChargingState::TOO_COLD;
        } else if (policy == BatteryChargingPolicy::LONG_LIFE) {
            health_info->chargingState = BatteryChargingState::LONG_LIFE;
        } else {
            health_info->chargingState = BatteryChargingState::NORMAL;
        }
    }

  private:
    void applyChargingPolicyLocked(int32_t level) {
        bool inhibit = (charging_policy_ == BatteryChargingPolicy::LONG_LIFE &&
                        level >= kLongLifeChargeLimitPct);
        if (inhibit == last_inhibit_written_) {
            return;
        }
        const char* val = inhibit ? "inhibit-charge" : "auto";
        if (::android::base::WriteStringToFile(val, kMp2650ChargeBehaviourPath)) {
            last_inhibit_written_ = inhibit;
        }
    }

    std::mutex policy_mutex_;
    BatteryChargingPolicy charging_policy_ = BatteryChargingPolicy::DEFAULT;
    int32_t last_level_ = 0;
    bool last_inhibit_written_ = false;
};

}  // namespace aidl::android::hardware::health::impl::kona

static constexpr const char* gInstanceName = "default";
static constexpr std::string_view gChargerArg{"--charger"};

int main(int argc, char** argv) {
#ifdef __ANDROID_RECOVERY__
    android::base::InitLogging(argv, android::base::KernelLogger);
#endif

    auto config = std::make_unique<healthd_config>();
    ::android::hardware::health::InitHealthdConfig(config.get());
    auto binder = ndk::SharedRefBase::make<
            aidl::android::hardware::health::impl::kona::HealthKona>(gInstanceName,
                                                                     std::move(config));

    if (argc >= 2 && argv[1] == gChargerArg) {
#if !CHARGER_FORCE_NO_UI
        return aidl::android::hardware::health::charger::ChargerModeMain(
                binder,
                std::make_shared<aidl::android::hardware::health::charger::ChargerCallback>(
                        binder));
#endif
        LOG(INFO) << "Starting charger mode without UI.";
    } else {
        LOG(INFO) << "Starting Kona health HAL.";
    }

    auto hal_health_loop =
            std::make_shared<aidl::android::hardware::health::HalHealthLoop>(binder, binder);
    return hal_health_loop->StartLoop();
}
