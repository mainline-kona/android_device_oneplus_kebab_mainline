# OnePlus 8T (`kebab`, SM8250) Mainline LineageOS 24.0 (Android 17) — Session Memory & Handoff

*Last updated: 2026-11-26*

---

## 1. Environment, Workspace & Rules

- **Device**: OnePlus 8T (`kebab`, SM8250 / Kona + external Qualcomm SDX55 5G modem over PCIe2/MHI)
- **ROM**: LineageOS 24.0 (Android 17) at `/serverhive5/zenzer0s/los`
- **Device Tree**: `/serverhive5/zenzer0s/los/device/oneplus/kebab_mainline` (branch `main`, HEAD `bc04e62`)
- **RIL Repo**: `/serverhive5/zenzer0s/los/hardware/mainline/kona-ril` (branch `empty`, HEAD `02a3d5b`)
- **Kernel**: `/serverhive5/zenzer0s/mainline-kona` (branch `android17-6.18-lts-8t`, ACK `android17-6.18-lts`, HEAD `86a20e7f8545`)
- **Reference Trees**:
  - Stock kebab kernel/DTS: `/serverhive5/zenzer0s/ref-stock-kebab`
  - Mainline reference: `/serverhive5/zenzer0s/mainline-kona-ref`
  - Stock vendor dump: `/serverhive5/zenzer0s/stock-vendor`
- **Live ADB Command**:
  ```bash
  export ADB_SERVER_SOCKET=tcp:127.0.0.1:5053
  /usr/bin/adb ...   # device serial 3b158d06
  ```
- **Trackers**:
  - Overall bring-up TODO: `device/oneplus/kebab_mainline/TODO.md`
  - Phased RIL bring-up tracker: `device/oneplus/kebab_mainline/RIL_BRINGUP.md`
- **Hard Rules / Commitments**:
  - **NEVER** run full Android builds (`m ...`) or flash images without asking — the user builds & flashes. (Compile-checking individual `.c`/`.cpp` files with Soong's clang flags to `/tmp/*.o` is fine and encouraged.)
  - **Commit locally**, never `git push` without asking.
  - **Never** edit upstream `arch/arm64/configs/defconfig`; kernel configs go in `arch/arm64/configs/kebab-android.config`.
  - Test SIM in slot 1 is **Jio India (`405-861`, `carrierId=2018`, 5G SA n78 + 4G LTE)** — note: test SIM currently has **no active data pack/recharge** (Jio walled garden allows DNS/ICMPv6 to `2405:200:800::11`, blocks external internet IPs).

---

## 2. Current Verified Working State (Flashed & Live-Tested)

- **Boot Time (~10.5 s `ISensors/default`)**:
  - Fixed 60 s SLPI boot stall (`b14096f`): `kebab-pdready.sh` runs `chown -R system:system /mnt/vendor/persist/sensors` + `vendor_hexagonrpcd.te` permissions.
- **Sensors / GNSS**:
  - Real mainline SSC sensors (`ISensors/default`) up at ~10.5 s.
  - GNSS (`gnss/gnss.c`, `c2bf6ba`): brings SDX55 modem `ONLINE` via `QMI_SERVICE_DMS` before starting QMI LOC.
- **Optical UDFPS Fingerprint (Goodix `gfp9638` G6_3) — Enrollment, Unlock & Persistence FIXED**:
  - **Root cause of reboot deletion (`b53743f`)**: OPlus Goodix HAL (`libgf_hal_19805_G6_3_oplus.so` / `G6_7`) sends all enrolled templates in ONE batch callback with **`msg->type == 11`** (`fingers[5]` at `+0x08`, `count` at `+0x30`, `gid` at `+0x34`), NOT standard AOSP `FINGERPRINT_TEMPLATE_ENUMERATING (6)`. Handled `type == 11` in `fingerprint/Session.cpp` -> **verified live**: `"Finger 1"` persists across flashing & reboot (`/data/system/users/0/settings_fingerprint_0.xml`), zero `"Set up Fingerprint Unlock again"` notifications!
  - **Speed optimizations (`21e0ad5`)**: `onUiReady()`-synced `GF_NET_EVENT_UI_READY` (60 ms fallback), persistent HBM during enrollment (`sEnrolling`), async HBM-off on unlock/finger-up, stock `#ffffe6` `config_udfpsColor`.
- **RIL Phase 0, 1, 2, 3 — ALL PASSED LIVE ON JIO 5G SA (`NR_SA`)**:
  - **Phase 0**: SDX55 PCIe2/MHI + Sahara v2 Mission Mode + `tqftpserv -d -i 3` MCFG + QRTR node 3 QMI services (`WDS`, `DMS`, `NAS`, `WMS`, `VOICE`, `UIM`, `LOC`, `WDA`, `IMSA`, `DSD`) + `/dev/wwan0at0`.
  - **Phase 1**: Kernel `CONFIG_RMNET=y` (`3a5bd1dd5b94`) + `kona-dataconn` (`825b90b`, `24a3df1`, `b008e30`): dynamic QRTR node 3 lookup, `mhi_hwip0`, `WDA_GET_DATA_FORMAT(PCIE, 4)` multi-APN guard, `WDS_SET_IP_FAMILY` per client, IPv6-first dual-stack ordering, raw TLV `0x2E` IPv6 P-CSCF extraction (tested live simultaneously on `jionet` `rmnet_data0` + `ims` `rmnet_data1` with both Jio IPv6 P-CSCFs).
  - **Phase 2**: `libcuttlefish-rild` + all 10 AIDL radio services registered, zero `rild` AVCs, Jio SIM `LOADED` (`carrierId=2018`, real IMEI/ICCID/IMSI/MSISDN), `IN_SERVICE` on **`NR_SA`** (`Jio True5G Jio`), 36-bit `CellIdentityNr` (`nci=4515725314`, `tac=20`), real QMI NAS 5G NR signal (`ssRsrp=-97 dBm, ssRsrq=-11 dB, ssSinr=+23 dB`, live bars), and clean Airplane Mode ON→OFF recovery (`d4751f7`).
  - **Phase 3**: Android Mobile Data + `clatd` 464XLAT (`rmnet_data0` Jio IPv6 MTU 1400 + `v4-rmnet_data0` `192.0.0.4/32` `Nat64Prefix: 64:ff9b::/96`), live 5G SA DNS (`12–25 ms`) + `ping6 2405:200:800::11` (`3/3`, `0% loss`, `32 ms`) with valid TCP/ICMPv6 checksums (`tcpdump` confirmed), clean teardown/reconnect across Airplane Mode.
  - **Phase 5 Foundation (Cold-Boot RIL, 5G↔4G Switching, QMI UIM ISIM, `generic-ims` Bound — ALL PASSED LIVE!)**:
    - **Cold-Boot RIL (`aabb70b`)**: `libcuttlefish-rild` came up cleanly on cold boot with zero restarts (`gsm.sim.state=LOADED`, `IN_SERVICE`, `kona-dataconn jionet` up automatically).
    - **Real 5G ↔ 4G LTE Switching (`07017e3`)**: `QMI NAS SetSystemSelectionPreference` (`0x0033`) switched SDX55 live between `NR_SA` (`bitmap=0x11ce0e -> mode_pref=0x005c`) and `LTE` (`bitmap=0x1ce0e -> mode_pref=0x001c`, `gsm.network.type=LTE`, `JIO 4G Jio`, LTE QMI signal `rssi=-67 rsrp=-97 rsrq=-9 rssnr=11 level=4` — full 4/4 bars!).
    - **QMI UIM ISIM Records (`07017e3`)**: `IsimUiccRecords` loaded `EF_ISIM_IMPI`, `EF_ISIM_DOMAIN`, `EF_ISIM_IST` live via QMI UIM (`0x0042`/`0x003F`/`0x003B`).
    - **Open AOSP VoNR + VoLTE + SMS-over-IMS (`02a6447`, `f0d5d02`) — REGISTERED LIVE!**:
      - `LteVopsSupportInfo: mVopsSupport = 2, mEmcBearerSupport = 2` -> `rmnet_data1` (`ims` PDN) came up automatically with real Jio IPv6 (`2409:4132:...`).
      - `requestSimAuthentication(QMI UIM): ctx=129 sw=9000 payload_len=44` succeeded!
      - `com.android.imsstack` **REGISTERED** (`registered=[mmtel smsip]`, `LTE: [voice ut sms]`, `NR: [voice sms]`, `IWLAN: [voice sms]`, `isImsRegistered=true`, `isVolteEnabled=true`, `isUtEnabled=true`).
    - **USIM FCP / `EF_SPN` (`a1bd6ff`) — PASSED LIVE!**: `[gsm.sim.operator.alpha]: [Jio]` + all 20+ Jio `ehplmns` loaded cleanly.
    - **AAC RichTap X-Axis LRA Haptics (`1734a29f39c9`, `c16a48a`, `f7ced20`) — PASSED LIVE!**:
      - `/dev/awinic_haptic` (`vendor_haptic_device`), `android.hardware.vibrator-service.kebab` (`hal_vibrator_default`, zero AVCs), calibrated resonant freq `174.3 Hz`, `AacRichTapDispEqualiza` streaming live, SystemUI crisp `TICK`/`CLICK` feedback & compositions completing in `18–29 ms`.

---

## 3. Recent Live-Verified Fixes & Ready to Flash Next

### Live-Verified on Phone (`6.18.63-mainline-gc224a9c6d728`)
1. **DSDS Dual-SIM + Dual IMEI/IMEI SV (`17`) + Full Baseband (`kona-ril` `8d962cc`, `66b6135`, `02a3d5b` + `kebab_mainline` `b9b1466`, `1a617d8`) — LIVE-VERIFIED!**
   - `com.android.phone` rock-solid > 1 hr with zero crashes (`ro.telephony.sim_slots.count=2` + `config_num_physical_slots=2` fixed `UiccController:1162 AIOOBE`).
   - `gsm.version.baseband = MPSS.HI.2.5-01066-SDX55_RMTEFS_PACK-1,MPSS.HI.2.5-01066-SDX55_RMTEFS_PACK-1`, real `slot0` + `slot1` IMEIs + `imeisv=17`, Jio `NR_SA` + `rmnet_data0`/`rmnet_data1` (IMS) up.
2. **Deep Sleep (`s2idle`) Fixes (`mainline-kona` `1a832994137e`, `ada17b334f19` + `kebab_mainline` `b890718`, `1a617d8`)**:
   - `fastrpc.c` `wait_event_freezable()`, SDX55 MHI `poll_work` suspend/resume + PME wakeup, DWC3 host-only wakelock (`pm_stay_awake`/`pm_relax`), `camcc-sm8250` runtime PM, `synaptics_s3908` `tcm_pm_ops` (`disable_irq`/`enable_irq`), `TARGET_SUPPORTS_SUSPEND := true`, dropped `clk_ignore_unused pd_ignore_unused regulator_ignore_unused`. (Still need unplugged/forced `AOSD`/`CXSD` count verification.)

### Committed Locally & Ready to Build/Flash Next (`boot.img` + `vendor.img`)
1. **`mainline-kona` (`86a20e7f8545`) — Fix Wi-Fi scan filtering bug in `ath11k` (`drivers/net/wireless/ath/ath11k/wmi.c`) — LIVE-VERIFIED**:
   - Populates `status->boottime_ns = ktime_get_boottime_ns()` for beacons and probe responses in `ath11k_mgmt_rx_event()`, matching `ath10k_wmi_event_mgmt_rx()`.
2. **`los/hardware/mainline/kona-ril` (`f37fda2`) — Fix QMI UIM logical channel leak (`0x0044`) breaking IMS/VoWiFi AKA auth**:
   - Persistent QRTR socket `s_qmiUimFd` + txn/indication filtering + `0x0044` reclaim/retry in `reference-ril.c`.
3. **RichTap Haptics Crispness & Sync Fixes (`mainline-kona` + `kebab_mainline`)**:
   - **`mainline-kona` (`aw8695-haptics.c` + `sm8250-oneplus-kebab.dts`)**:
     - Switch AW8697 boost peak current limit from `2.0A` to stock `3.5A` (`AW8695_BSTCFG_PEAKCUR_3P5A`).
     - Restore factory laser-trimmed eFuse 24 MHz oscillator (`AW8695_ANACTRL_LRA_SRC_EFUSE`) for RichTap RTP/Stream playback instead of writing `TRIM_LRA=0` while `LRA_SRC_REG` was left stuck from `aw8695_haptic_get_f0()`, and disable LPF (`AW8695_DATCTRL_LPF_DISABLE`) after f0 detection.
     - Add FIFO-aware `aw8695_rtp_write_fifo()` (512B chunks, `FF_AFS` pacing for >2 KB payloads), faster 500 us standby poll, and skip IRQ I2C access while `haptic_rtp_mode` is active.
     - Fix OnePlus 8T AW8697 IRQ pin in `sm8250-oneplus-kebab.dts` from `gpio118` (8/8Pro) to `gpio11`.
   - **`kebab_mainline` (`vibrator/Vibrator.{h,cpp}` + `OPlusFrameworksResTarget` / `OPlusSystemUIResTarget`)**:
     - Sync with stock `hardware/oplus` RichTap HAL: start `aac_vibra_looper_start()`, use `aac_vibra_looper_prebaked_effect()` / `aac_vibra_looper_on()`, fix exact `aac_richtap.config` IDs/durations (`DOUBLE_CLICK=0x1001/85ms`, `THUD=0x3001/36ms`, `HEAVY_CLICK/SLOW_RISE=0x3002/30ms`, `POP/SPIN=0x3004/24ms`, `CLICK=0x3009/12ms`, `TICK=0x300a/12ms`, `LIGHT_TICK=0x300b/11ms`, `NOOP=0ms`), unity-gain strengths (`50/80/100`), full-amplitude reset before effects, drop fake `CAP_GET_Q_FACTOR`, and stop calling `aac_vibra_stop()` before every click.
     - Add `kRtpPipelineMarginMs` (15 ms) before firing `callback->onComplete()` on short RTP bursts so `VibratorManagerService`'s trailing `off()` -> `RICHTAP_STOP_MODE` never cuts off the active braking tail mid-waveform.
     - Add stock OnePlus 8T haptic intensity/keyboard/text-handle/icon-animation RRO configs.

**Next Build Commands for User (`boot.img` + `vendor.img`)**:
```bash
m bootimage vendorimage
```
(then flash `boot.img` and `vendor.img`).

---

## 4. Immediate Next Steps When Resuming

1. **Live-Test VoLTE / VoNR Registration & Calls (Phase 4 & Phase 5)** after flashing `vendor.img`:
   - Verify `DataNetworkController` allows `IMS&MMTEL` PDN -> `kona-dataconn ims ipv4v6 2` brings up `rmnet_data1` with Jio IPv6 + P-CSCFs.
   - Verify `com.android.imsstack` authenticates via QMI UIM ISIM AKA (`requestSimAuthentication`) and registers `MMTEL` (`VOICE` + `SMS`, `HD`/`VoLTE` icon).
   - Test incoming/outgoing VoLTE call (2-way VoIP-mode audio via `config_use_voip_mode_for_ims=true`) + SMS (Phase 4 & SMS-over-IMS).
2. **UDFPS Sensor-Only HBM Dimming (User Requested Earlier — Pending Choice)**:
   - `amb655x` only has Global HBM (no hardware Local HBM).
   - Stock `amb655x` brightness-to-alpha LUT is already located at `/serverhive5/zenzer0s/ref-stock-kebab/kernel/arch/arm64/boot/dts/vendor/oplus/kebab/dsi-panel-oplus20828-samsung-amb655x-1080-2400-120fps.dtsi` (`oplus,dsi-fod-brightness`, lines 671–691), and LineageOS 24.0 SystemUI has native `UdfpsHelper.kt` (`config_udfpsFrameworkDimming`).
   - Two clean options to implement when user is ready:
     1. **Press-only Dim Layer** (small SystemUI `UdfpsHelper` tweak so dim layer shows *only* while finger is down / HBM active — zero lockscreen/shade dimming side effects), OR
     2. **Overlay-synced HBM + `config_udfpsFrameworkDimming=true`** (vendor/odm only, keeps HBM+dim synced while overlay is visible, guarding AOD/shade).
3. **Phase 6 & Remaining Items (see `TODO.md` & `RIL_BRINGUP.md`)**:
   - Legacy CS Voice (`q6voice` + VoiceMMode1 DAI links + `kona-voicecall`) for non-VoLTE fallback.
   - Dual-SIM (`slot2` DSDS) if needed.
   - EFS writeback evaluation (`sahara_mdm.efs_writeback=1`) & SELinux Enforcing audit (e.g. `hal_fingerprint_default` `/dev/tee0` & `/mnt/vendor/persist/engineermode` AVCs noted in boot logcat).
