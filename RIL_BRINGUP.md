# OnePlus 8T (`kebab`) Mainline RIL & Telephony Bring-Up Plan

Target: **LineageOS 24.0 (Android 17)** on **ACK `android17-6.18-lts`** (`mainline-kona`, branch `android17-6.18-lts-8t`), external **Qualcomm SDX55 5G modem** over PCIe2/MHI.

---

## 1. Verified Hardware & Live Ground Truth

Captured directly from live hardware (`MPSS.HI.2.5-01066-SDX55_RMTEFS_PACK-1.5472.139.63951.2`, Jio India SIM in Slot 1):

### Kernel / MHI / IPC Layer (Verified Working)
- **PCIe2 + MHI (`mhi-pci-generic`, `sahara_mdm`)**:
  - SDX55 boots cleanly to Mission Mode via in-kernel Sahara v2 loader (`sdx55m/sbl1.mbn` + `qdsp6sw.mbn` + `@mdm1oemnvbktmp` OEM RF NV).
  - `mhi0` channels exposed:
    - Ch 4/5 `DIAG` -> `/dev/wwan0qcdm0`
    - Ch 10/11 `EFS` -> `sahara_mdm` (`efs_writeback=0` safety during bring-up)
    - Ch 14/15 `QMI` -> `/dev/wwan0qmi0` (`0660 radio:radio`)
    - Ch 20/21 `IPCR` -> `qrtr_mhi` -> **QRTR Node 3**
    - Ch 32/33 `DUN` -> `/dev/wwan0at0` (`0660 radio:radio`)
    - Ch 100/101 `IP_HW0` (UL 128 / **DL 1024** ring, `6d0de5b9`) -> `mhi_net` -> **`mhi_hwip0`** (`mtu 16384`, `ARPHRD_RAWIP`)
- **TFTP MCFG Serving (`tqftpserv -d -i 3`)**:
  - Verified live serving `657_0_0.mbn`, `mbn_hw.dig`, `mbn_sw.dig` from `/vendor/firmware_mnt/image/modem_pr/`.
- **Missing Kernel Config for Multi-APN**:
  - **`CONFIG_RMNET`** is **not enabled** yet in `kebab-android.config` (source exists at `drivers/net/ethernet/qualcomm/rmnet/`). Required for QMAPv5 multiplexing (`rmnet_data0` internet + `rmnet_data1` ims simultaneously on top of `mhi_hwip0`).
- **Missing Kernel Driver for Legacy 2G/3G CS Voice**:
  - `q6voice` is absent from `mainline-kona` (`q6voice-*.bin` blobs exist in `kebab_mainline-firmware/qcom/`). **Note:** VoLTE/VoNR/VoWiFi via AOSP `ImsStack` + `ImsMedia` uses **`config_use_voip_mode_for_ims=true`** (standard AP VoIP audio over existing working ADSP `MultiMedia1`/`MultiMedia3`), so VoLTE does **not** depend on `q6voice`.

### Live QRTR Node 3 QMI Services (Verified)
| Service | ID | Status on Live SDX55 | Notes |
|---|---|---|---|
| `WDS` | 1 | OK | 3 3GPP profiles (`1`=jionet, `2`=ims, `3`=sos) |
| `DMS` | 2 | OK | IMEI `861235050863059`, starts in mode `5 (shutting-down)` at boot until set `ONLINE` |
| `NAS` | 3 | OK | Serving system, LTE + 5G NR signal info (`RSRP`/`RSRQ`/`SINR`), RF bands |
| `WMS` | 5 | OK | SMS over QMI (useful for LTE/IMS SMS fallback) |
| `VOICE` | 9 | OK | CS & modem voice control |
| `UIM` | 11 | OK | 2 physical slots: slot 1 PRESENT (`USIM` READY + `ISIM` DETECTED), slot 2 ABSENT |
| `LOC` | 16 | OK | Verified working in `kona-gnss` |
| `WDA` | 26 | OK | **Requires `endpoint_info` TLV (`PCIE`, `ep_index=4`)** (returns `InvalidArgument (48)` without it) |
| `IMSA` | 33 | OK | Modem IMS status (optional; AOSP `ImsStack` runs open SIP/IMS on AP) |
| `DSD` | 42 | OK | Data System Determination |

### Live SDX55 AT Command Support (`/dev/wwan0at0`, Verified)
- **Working**: `ATI` (IMEI/rev), `AT+CFUN?`/`AT+CFUN=1`, `AT+CPIN?` (`READY`), `AT+CIMI`, `AT+CSQ` (`26,99`), `AT+COPS?` (`"JIO 4G Jio",7`), `AT+CREG?` (`0,1`), `AT+CEREG=?` (`0-2`, reg `0,1`), `AT+C5GREG=?` (`0-2`), `AT+CRSM=?` (`OK`), `AT+CNMI=?` (`(0,1,2),(0,1,2,3),(0,2),(0,1,2),(0,1)`), `AT+CGDCONT?`, `AT+CGACT?`, `AT+CGPADDR` (Jio IPv6 `2409:40f2:...`).
- **Unsupported (`ERROR` on SDX55)**:
  - **`AT+CCHO` / `AT+CGLA`** -> **CRITICAL**: SDX55 AT does **not** support logical channels / generic SIM APDU! Must be implemented via **`QMI_SERVICE_UIM`** (`open_logical_channel` / `send_apdu` / `logical_channel`) for IMS ISIM AKA authentication (VoLTE)!
  - **`AT+CESQ` / `AT+QCSQ`** -> Not supported; LTE/5G NR RSRP/RSRQ/SINR signal metrics should come from **`QMI_SERVICE_NAS`** (`get_signal_info`) instead of coarse `AT+CSQ` alone.

---

## 2. Architecture Decision

**Hybrid Android 17 AIDL v5 RIL (`hardware/mainline/kona-ril`) + Native QMI Data/UIM/NAS + Open AOSP IMS (`hardware/lineage/generic-ims`)**:
1. **Radio AIDL HAL (`hardware/mainline/kona-ril`)**:
   - Ported from `mainline-kona-ref/android_hardware_mainline_kona-ril` (Cuttlefish AIDL v5/v4 daemon `libcuttlefish-rild` + `libcuttlefish-ril-2.so` over `/dev/wwan0at0`).
   - Implements all Android 17 `compatibility_matrix.202604.xml` interfaces: `IRadioConfig/5`, `IRadioData/5`, `IRadioMessaging/5`, `IRadioModem/5`, `IRadioNetwork/5`, `IRadioSim/5`, `IRadioVoice/5`, `ISap/1`, `IRadioIms/4`, `IImsMedia/4`.
   - **QMI-backed subsystems** over QRTR node 3 (`libqmi-glib` + `libqrtr-glib`, already in `/vendor/lib64/`):
     - **Data plane**: `kona-dataconn` using `WDA` (Raw-IP + QMAPv5 on `PCIE` ep 4) + kernel `rmnet` (`rmnet_data0..N` over `mhi_hwip0`) + `WDS` (`bind_mux_data_port` + dual-stack IPv4/IPv6 `start_network` + P-CSCF).
     - **SIM APDU / ISIM AKA (Phase 5)**: `QMI_SERVICE_UIM` replacing unsupported `AT+CCHO`/`AT+CGLA`.
     - **Signal Strength / 5G NR (Phase 2)**: `QMI_SERVICE_NAS` `get_signal_info` supplementing `AT+CSQ`.
2. **Open IMS Stack (`hardware/lineage/generic-ims/ims.mk`)**:
   - Ships AOSP `ImsStack` (`com.android.imsstack`), `ImsMediaService` (`libimsmedia`), `Iwlan`, `QualifiedNetworksService`, and `CarrierSettings` (includes `rjio_in.pb`, `airtel_in.pb`, `vodafone_in.pb`, `idea_in.pb`, `bsnl_in.pb`).
   - Uses `config_use_voip_mode_for_ims=true` so VoLTE/VoWiFi calls use normal AP VoIP audio paths.

### Known Bugs Found Ahead of Time (Must Fix During Bring-Up)
1. `mainline-kona/arch/arm64/configs/kebab-android.config`: missing `CONFIG_RMNET=y`.
2. `kona-ril/dataconn/dataconn.c`:
   - Hardcodes `qrtr_bus_wait_for_node(bus, 0, ...)` -> must dynamically find `QMI_SERVICE_WDS` node (fallback **node 3**).
   - `get_parent_iface()` misses **`mhi_hwip0`**.
   - Calls `WDA_SET_DATA_FORMAT` unconditionally on every APN start -> must check `WDA_GET_DATA_FORMAT` (with `endpoint_info` PCIe ep 4) first so bringing up a 2nd APN (`ims`) doesn't fail or reset the 1st APN (`jionet`).
   - Needs `ensure_modem_online()` safety check if DMS is still `shutting-down`.
3. `kona-ril/guest/hals/ril/reference-ril/reference-ril.c`:
   - `AT+CCHO` / `AT+CGLA` return `ERROR` on SDX55 -> must use `QMI_SERVICE_UIM` for logical channels & APDU.
   - `AT+CESQ` / `AT+QCSQ` return `ERROR` -> supplement signal reporting with `QMI_SERVICE_NAS`.
   - Verify IPv6-only & dual-stack `setupDataCall` handling (Jio assigns IPv6 + IPv4v6).
4. `device/oneplus/kebab_mainline/rro/FrameworksResTarget/res/values/config.xml`:
   - Still sets `config_qualified_networks_service_package` & `config_wlan_network_service_package` to `vendor.qti.iwlan` -> remove so `FrameworkResOverlayIms` takes effect cleanly.
5. `device/oneplus/kebab_mainline/sepolicy/vendor/file_contexts`:
   - Missing `/vendor/bin/hw/libcuttlefish-rild u:object_r:rild_exec:s0`.

---

## 3. Phased Bring-Up Plan & Live Test Checklist

### Phase 0 — Modem Hardware & IPC Baseline
- [x] SDX55 PCIe2/MHI + Sahara v2 boot to Mission Mode
- [x] `tqftpserv -d -i 3` serving MCFG (`657_0_0.mbn`, `mbn_hw.dig`, `mbn_sw.dig`)
- [x] QRTR node 3 QMI census (DMS, UIM, NAS, WDA, WDS, WMS, VOICE, IMSA, DSD)
- [x] Live AT probe confirming SIM `READY`, `CSQ=26`, LTE Home registration on `"JIO 4G Jio"`

### Phase 1 — Kernel `CONFIG_RMNET` & Standalone QMI Data Plane (`kona-dataconn`)
**Goal**: Prove rock-solid, dual-stack, multi-APN (`jionet` + `ims`) packet data directly over `mhi_hwip0` + `rmnet_data0/1` before involving Android Telephony.
- [x] Enable `CONFIG_NET_VENDOR_QUALCOMM=y` + `CONFIG_RMNET=y` in `mainline-kona/arch/arm64/configs/kebab-android.config` (`3a5bd1dd5b94`, compile-verified `drivers/net/ethernet/qualcomm/rmnet/built-in.a`).
- [x] Clone `mainline-kona-ref/android_hardware_mainline_kona-ril` into `los/hardware/mainline/kona-ril`.
- [x] Fix `kona-ril/dataconn/dataconn.c` & `Android.bp` (`825b90b`, compiled cleanly with `-Wall -Wextra`):
  - Dynamic QRTR node lookup for `QMI_SERVICE_WDS` (fallback node 3) + `ensure_modem_online()` via `QMI_SERVICE_DMS`.
  - Add `"mhi_hwip0"` as primary parent interface in `get_parent_iface()`.
  - Query `WDA_GET_DATA_FORMAT` with `endpoint_info (PCIE, 4)` first; skip `WDA_SET_DATA_FORMAT` when already configured for Raw-IP + QMAPv5.
  - Extract IPv4 + IPv6 addresses, gateways, DNS, P-CSCF (IPv4 + domain + raw TLV `0x2E` IPv6 P-CSCF), MTU, and clean QMI client/link teardown on `SIGTERM`.
- [x] **Phase 1 Live Test — PASSED on real Jio 5G SA (NR Band n78, ch 42912, RSRP -99 dBm, SNR 19.0 dB)**:
  - [x] `kona-dataconn jionet ipv4v6 1` -> `rmnet_data0@mhi_hwip0` up with real Jio IPv6 (`2409:40f2:35e:768c::/64`), gateway, DNS (`2405:200:800::11`), MTU `1400`, received live SLAAC Router Advertisement.
  - [x] `ping6 2405:200:800::11` over `rmnet_data0` -> **3/3 received, 0% packet loss, 31.5 ms min RTT**, zero RX/TX errors/drops on `mhi_hwip0` & `rmnet_data0`!
  - [x] Simultaneously ran `kona-dataconn ims ipv4v6 2` -> `rmnet_data1@mhi_hwip0` up with real Jio IMS IPv6 (`2409:4132:...`) + **both real IPv6 P-CSCF servers** (`2405:200:642:b471:21::4c`, `2405:200:607:b471:21::112`) extracted via TLV `0x2E`!
  - [x] `SIGTERM` cleanly tore down both `rmnet_data0` and `rmnet_data1` with zero leaked interfaces.
  - *Key fix discovered during live test*: Set `QMI_WDS_SET_IP_FAMILY` per WDS client and start IPv6 before IPv4 on dual-stack carriers (`ipv4v6`), plus report negotiated `protocol=IPV6|IP|IPV4V6` so Android's `clatd` (464XLAT) activates automatically on IPv6-only carriers like Jio.

### Phase 2 — Core AIDL v5 RIL (Radio Power, SIM, Registration, Signal)
**Goal**: Android Telephony binds to `IRadio*` AIDL v5 services, detects Jio SIM, shows carrier name & real LTE/5G signal bars, and cleanly toggles Airplane Mode.
- [x] Wire `kona-ril` into `device/oneplus/kebab_mainline/device.mk`:
  - `PRODUCT_PACKAGES += libcuttlefish-rild kona-dataconn`
  - Install `android.hardware.telephony.{calling,data,gsm,messaging,radio.access,subscription}.xml` permissions.
- [x] Add `/vendor/bin/hw/libcuttlefish-rild u:object_r:rild_exec:s0` to `sepolicy/vendor/file_contexts` and drop stale `vendor.qti.iwlan` strings from `rro/FrameworksResTarget`.
- [x] Patch `reference-ril.c` & `ril_service.cpp` for SDX55 LTE + 5G SA:
  - Enable `AT+CEREG=2` & `AT+C5GREG=2` on init and handle `+CEREG:`/`+C5GREG:` unsolicited notifications.
  - Fix `parseRegistrationState()` to accept `>= 4` commas (`+C5GREG: 2,1,"14","10D288002",11,4,"01.000001"` with 36-bit NR NCI and extended `+CEREG:`) and fall back to `+CEREG?`/`+C5GREG?` for both data & voice on LTE/5G-only networks like Jio.
  - Add raw `AF_QIPCRTR` **QMI `NAS_GET_SIGNAL_INFO` (`0x004F`)** support in `readSignalStrength()` (verified live: LTE RSRP/RSRQ/RSSI/SNR & 5G NR RSRP/RSRQ/SINR) + fix 10 s `pollSignalStrength()` timer & `INT_MAX` unused-field initialization.
  - Fix `ril_service.cpp` `convertToPdpProtocolType()` (`strcmp` for `IPV4V6`/`IPV6`/`IP` instead of `strncmp("IP", ..., 2)` which misclassified `IPV6`/`IPV4V6` as IPv4-only!) + add `RADIO_TECH_NR` `V1_5::CellIdentityNr` support.
- [x] **Phase 2 Live Test — PASSED on Jio 5G SA (`NR_SA`)**:
  - [x] `service list` shows all `android.hardware.radio.*` AIDL services registered (10/10).
  - [x] `logcat -b radio` shows clean `RILJ` <-> `RILD` handshake, Jio SIM `LOADED` (`carrierId=2018`), real IMEI/ICCID/IMSI/MSISDN populated, zero `rild` AVCs.
  - [x] `mVoiceRegState=0(IN_SERVICE)`, `mDataRegState=0(IN_SERVICE)`, `gsm.network.type=NR_SA`, `Jio True5G Jio` (`405-861`), real 36-bit `CellIdentityNr` (`nci=4515725314`, `tac=20`).
  - [x] QMI `NAS_GET_SIGNAL_INFO` (`QRTR node=3 port=57`) reporting real 5G NR signal (`ssRsrp=-97 dBm`, `ssRsrq=-11 dB`, `ssSinr=23 dB`, `level=1`).
  - [x] Airplane Mode ON -> OFF cleanly drops to `POWER_OFF` (tearing down `rmnet_data0` + `v4-rmnet_data0`) and auto-recovers back to `IN_SERVICE (NR_SA)` + reconnects data.
  - [x] Fixed boot-time `AT+CFUN=1` race in `initializeCallback()`/`requestRadioPower()` (`stopDataconn(0)` on radio-off, keep `UNAVAILABLE` during AT init, skip redundant `AT+CFUN=1` when modem is already online).

### Phase 3 — Android Mobile Data Integration
**Goal**: Native Android mobile data works end-to-end (browser, apps, Wi-Fi <-> Mobile handoff, Airplane mode recovery).
- [x] Audited `reference-ril.c` & `ril_service.cpp` `setupDataCall` / `deactivateDataCall` / `convertToPdpProtocolType`:
  - Fixed `IPV6`/`IPV4V6` PDP protocol mapping in `ril_service.cpp` so Android 464XLAT (`clatd`) activates automatically on IPv6-only Jio (`v4-rmnet_data0`, `192.0.0.4/32`, `Nat64Prefix: 64:ff9b::/96`).
- [x] Verified `rild.te` SELinux permissions for `kona-dataconn` netlink/ioctl/QRTR/sysfs_net (zero `rild` AVCs).
- [ ] **Phase 3 Live Test** (in progress):
  - [x] `svc data enable` brings up `rmnet_data0@mhi_hwip0` (Jio IPv6 + MTU 1400) + `v4-rmnet_data0` (464XLAT) automatically in ~0.4 s; Airplane Mode toggle tears down and reconnects cleanly with zero leaked interfaces.
  - [x] Live 5G SA IPv6 DNS (`www.google.com` 25 ms, `connectivitycheck.gstatic.com` 12 ms) and `ping6 2405:200:800::11` (3/3, 0% loss, 32 ms) verified with valid checksums (`tcpdump` confirmed).
  - [ ] External internet HTTP/HTTPS validation (confirm active data pack on SIM / test browsing with Wi-Fi OFF).

### Phase 4 — SMS (Send & Receive)
**Goal**: Reliable SMS send & receive in LineageOS Messaging (including bank/app OTPs).
- [ ] Audit `reference-ril.c` `IRadioMessaging/5` (`AT+CMGF=0`, `AT+CNMI`, `AT+CMGS`, `+CMT`, `acknowledgeIncomingGsmSms`).
- [ ] Test SMS on Jio LTE; if Jio rejects CS/NAS AT SMS and requires SMS-over-IMS or QMI WMS, wire QMI `WMS` / Phase 5 IMS SMS support cleanly.
- [ ] **Phase 4 Live Test**:
  - [ ] Send an SMS to another phone -> delivered cleanly.
  - [ ] Receive an SMS (and multipart SMS) -> arrives immediately in Messaging with screen on and screen off.

### Phase 5 — VoLTE / VoNR / VoWiFi (Open AOSP `generic-ims` + QMI UIM APDU)
**Goal**: Full IMS registration (`MMTEL VOICE` + `SMS`) and 2-way VoLTE voice calls on Jio 4G/5G (and VoWiFi)!
- [ ] Implement **QMI UIM logical channel & APDU** (`QMI_SERVICE_UIM`: `open_logical_channel`, `close_logical_channel`, `send_apdu`, plus exposing `ISIM` app in `getIccCardStatus`) in `kona-ril` so `ImsStack` can perform ISIM AKA authentication (since SDX55 AT returns `ERROR` for `AT+CCHO`/`AT+CGLA`).
- [ ] Wire `hardware/lineage/generic-ims/ims.mk` into `device.mk` (`ImsStack`, `ImsMediaService`, `Iwlan`, `QualifiedNetworksService`, `CarrierSettings` incl. `rjio_in.pb`).
- [ ] Fix `rro/FrameworksResTarget/res/values/config.xml` (drop conflicting `vendor.qti.iwlan` strings) and verify `TelephonyOverlayIms` (`config_ims_mmtel_package=com.android.imsstack`, `config_use_voip_mode_for_ims=true`).
- [ ] Verify `ims` PDN (`rmnet_data1`, mux 2) brings up IPv6/IPv4 + P-CSCF addresses and passes them to Android `DataCallResponse.pcscfAddresses`.
- [ ] **Phase 5 Live Test**:
  - [ ] `dumpsys ims` shows `ImsStack` bound, `MMTEL` **REGISTERED**, `VOICE` + `SMS` capabilities available.
  - [ ] Status bar shows `VoLTE` / `HD` icon; SIM settings show VoLTE & Wi-Fi Calling toggles.
  - [ ] Place & receive real VoLTE calls on Jio -> clear 2-way audio on earpiece, speakerphone, mute, DTMF, and clean hangup!
  - [ ] Test SMS-over-IMS and VoWiFi (optional).

### Phase 6 — Polish: Legacy CS Voice (`q6voice`), Dual-SIM (`slot2` DSDS), EFS & SELinux
- [ ] **Legacy 2G/3G CS Voice** (for non-VoLTE fallback / CSFB carriers): port `q6voice` driver + `sm8250-oneplus-common.dtsi` VoiceMMode1 DAI links + `q6voice-*.bin` + `audio/ucm2` VoiceCall routing + `kona-voicecall`.
- [ ] **Dual-SIM (DSDS)**: expose `slot2` AIDL instances when `ro.boot.opt_sim_count=2` (if second physical SIM needed).
- [ ] **EFS Writeback & Cleanup**: evaluate `sahara_mdm.efs_writeback=1` once modem stability is proven; clean up `bringup-log.sh` `kebab-dataconn --diag-msgs` reference; verify zero `rild`/`ims` AVCs for SELinux Enforcing.

---

## 4. Progress Log

| Date | Phase | Milestone / Finding |
|---|---|---|
| 2026-11-25 | Phase 0 | Verified `tqftpserv -i 3` MCFG load, QRTR node 3 QMI services (DMS/UIM/NAS/WDA/WDS/WMS/VOICE/IMSA/DSD), live Jio 4G LTE registration (`CSQ=26`, `CEREG=0,1`), and identified SDX55 AT gaps (`AT+CCHO`/`AT+CGLA`/`AT+CESQ` return `ERROR`) + missing kernel `CONFIG_RMNET`. Created `RIL_BRINGUP.md`. |
| 2026-11-25 | Phase 1 | **PASSED LIVE**: `CONFIG_RMNET=y` + fixed `kona-dataconn` brought up simultaneous `jionet` (`rmnet_data0`, Jio 5G SA n78 IPv6, 31.5 ms RTT, 0% loss) + `ims` (`rmnet_data1` + real IPv6 P-CSCF via TLV `0x2E`) and cleanly tore down on `SIGTERM`. |
| 2026-11-25 | Phase 2+3 | Wired `libcuttlefish-rild` + `kona-dataconn` + telephony permissions + `rild_exec` sepolicy + 5G SA `C5GREG` + negotiated `protocol=` reporting into `device.mk` & `kona-ril`. Ready to build & live-test (`m vendorimage odmimage`). |
