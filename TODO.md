# OnePlus 8T (`kebab`) Mainline Bring-Up TODO

Target: LineageOS 24.0 (Android 17) on ACK `android17-6.18-lts` (`mainline-kona`, branch `android17-6.18-lts-8t`).

---

## 1. Ready to Build, Flash & Verify Now (`m bootimage vendorimage`)

Local commits ready across `mainline-kona`, `device/oneplus/kebab_mainline`, and `hardware/mainline/qcom`:

- [ ] **Rebuild & flash** `boot.img` and `vendor.img` (`m bootimage vendorimage`).
- [ ] **Verify fast boot (~18–22 s vs ~79 s)** (`8068c3a`):
  - Confirm `qrtr-cfg 1`, `hexagonrpcd-{sdsp,adsp-rootpd}`, and `start_remoteproc.sh` bring SLPI up in `post-fs` (~4.8 s) before `vendor.sensors-mainline` starts (~7.3 s).
  - Confirm `ISensors/default` registers in ~200 ms with all 6 SLPI sensors and no 62 s `ams_ready` stall.
- [ ] **Verify Goodix optical UDFPS enrollment & unlock** (`722fbc69aa3e`, `6e61e44b2b4f`, `d049c8c`, `28c3aca`):
  - Confirm `/proc/touchpanel/fp_enable` exists and `goodix::Device::enable_tp(1)` succeeds.
  - Confirm `HBM=1 (25 ms) -> TOUCHDOWN -> notify_fppress=1 (GF_NET_EVENT_UI_READY)` triggers clean optical capture with no `GF_ERROR_UI_READY_TIMEOUT`.
  - Test full enrollment in Settings, lockscreen unlock, and biometric prompt.
- [ ] **Verify SDX55 modem RF & GPS** (`328719f`, `03ee542`):
  - Confirm `tqftpserv` serves `/vendor/firmware_mnt/image/modem_pr/mcfg/...` cleanly (no truncated `/vendor/firmware_mnt/imag` `ENOENT`).
  - Confirm SDX55 boots out of `DMS_OP_MODE: offline` and `kona-gnss` gets `ENGINE_STATE: ON`, live SV/GSV satellites (GPS/GLONASS/Galileo/BeiDou/QZSS), and a real GPS fix.

---

## 2. Feature Ports from `oplus-kona/android_hardware_oplus` & Downstream Kernel

- [ ] **RichTap X-Axis LRA Haptics (Replace generic `input-ff` rumble)**:
  - Port downstream `drivers/misc/aw8697_haptic` (`aw8697.c`, `/dev/awinic_haptic`, `/sys/class/leds/vibrator/*`) to Linux 6.18 on `haptics@5a` (`sm8250-oneplus-common.dtsi`), replacing `aw8695-haptics.c`.
  - Port `aidl/vibrator/richtap` from `oplus-kona/android_hardware_oplus`.
  - Pull `libaacvibrator.so` and `aw8697_*_RTP_*.bin` waveforms from `stock-vendor/sm8250-common`.
  - Add ueventd + SELinux rules for `/dev/awinic_haptic` and `/sys/class/leds/vibrator/*`.
- [ ] **Alert Slider (3-Position Tri-State Key)**:
  - Port downstream Hall-sensor driver (`tri_state_key` / `/proc/tristatekey/tri_state` + input key events) to `mainline-kona` + DTS node in `sm8250-oneplus-kebab.dts`.
  - Port `packages/KeyHandler` from `oplus-kona/android_hardware_oplus` (`KebabMainlineKeyHandlerResTarget` and `config_deviceKeyHandlerLibs` are already configured).
- [ ] **Ambient Display Gestures (`packages/Doze` / `OplusDoze`)**:
  - Port `packages/Doze` from `oplus-kona/android_hardware_oplus` for Pick-up to Wake, Hand Wave, and Pocket Mode using our working SLPI sensors (`bmi26x` + `stk33502`).
- [ ] **LiveDisplay (`aidl/livedisplay`: HBM, DC Dimming, Color Profiles)**:
  - Add downstream `amb655x` DSI sequences for `seed` (sRGB / DCI-P3) and `dimlayer_bl_en` (DC Dimming) to `drivers/gpu/drm/panel/panel-samsung-amb655x.c` (alongside existing `hbm`).
  - Port `aidl/livedisplay` from `oplus-kona/android_hardware_oplus`.
- [ ] **Touchscreen Gestures / High Touch Polling (Optional)**:
  - Add double-tap-to-wake / screen-off gesture and high-polling hooks to mainline Synaptics S3908/RMI4 touchscreen driver and wire `aidl/touch`.

---

## 3. Core Bring-Up & Daily-Driver Polish

- [ ] **UDFPS Polish (if needed after live test)**:
  - Tune `config_udfpsColor` (`#ffffe6` warm white in `KebabMainlineOPlusSystemUIResTarget`, if optical SNR/contrast benefits over `#ffffffff`).
  - Optional screen-off UDFPS wake gesture (requires touchscreen wake hook + `org.lineageos.sensor.udfps` sensor).
- [ ] **Cellular / RIL (External SDX55 over MHI)**:
  - Verify modem online state after `tqftpserv` fix.
  - Bring up QMI RIL / SIM / mobile data (`mhi_hwip0`) / CS-VOICE (`kona-voicecall`) / IMS.
  - Once modem boot stability is proven, re-evaluate `sahara_mdm.efs_writeback=0` in `init.kebab.rc`.
  - Optional: port `nrmodeswitcher` (5G SA/NSA toggle) from `hardware_oplus`.
- [ ] **Deep Sleep / Suspend & Resume**:
  - Debug USB gadget drop and hang on resume, then switch `TARGET_SUPPORTS_SUSPEND := true` in `device.mk`.
- [ ] **Camera**:
  - Bring up camera sensors / ISP / HAL.
- [ ] **SELinux Enforcing**:
  - Clean up remaining permissive AVCs (`kebab-pdready.sh` domain transition, `pd-mapper` `firmware_class` sysfs read, `vendor_start_remoteproc`, `system_suspend` wakeup sysfs, etc.) and switch from Permissive to Enforcing.
- [ ] **Release Cleanup**:
  - Disable bring-up background loggers (`vendor.bringup_kmsg`, `bringup_logcat`, `bringup_diag`) once stable.
  - Review and push local commits across `mainline-kona`, `device/oneplus/kebab_mainline`, `hardware/mainline/qcom`, and `external/qcomtee`.

---

## 4. Already Verified Working

- ACK `android17-6.18-lts` GKI kernel boot on OnePlus 8T (`kebab`).
- Display & GPU: `amb655x` 120 Hz / 60 Hz DSC panel + Mesa Turnip Vulkan 1.3 (`skiavk` + `skiavkthreaded`).
- Wi-Fi (`ath11k_pci` QCA6390 with `post-fs` rebind) & Bluetooth (`QCA6390` + `bdaddr`).
- Audio (ADSP + UCM2) & Hardware Video Codecs (Venus V4L2 Codec2).
- NFC (`android.hardware.nfc2-service.nxp`).
- All 6 SLPI hardware sensors via `libssc` (`bmi26x` accel/gyro, `mmc5603x` mag, `stk33502` light/prox, Rotation Vector).
- Hardware-backed Keymaster 4.1 + Gatekeeper 1.0 + UFS RPMB + Lockscreen PIN/Pattern (`qcomtee` + `qseecomd`).
- Goodix `gfp9638` TA load, QTEE DMA32/CMA `< 4 GiB` SHM, unlocked calibration (`vendor.fingerprint.cali=1`), AIDL `IFingerprint/default`.
- Idle CPU & thermals fixed (~86–93% idle, ~50–54 °C after removing legacy HIDL `config_biometric_sensors`).
