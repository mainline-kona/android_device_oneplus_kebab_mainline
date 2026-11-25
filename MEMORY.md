# OnePlus 8T (`kebab`, SM8250) Mainline LineageOS 24.0 (Android 17) — Session Memory & Handoff

*Last updated: 2026-11-25*

---

## 1. Environment, Workspace & Rules

- **Device**: OnePlus 8T (`kebab`, SM8250 / Kona + external Qualcomm SDX55 5G modem over PCIe2/MHI)
- **ROM**: LineageOS 24.0 (Android 17) at `/serverhive5/zenzer0s/los`
- **Device Tree**: `/serverhive5/zenzer0s/los/device/oneplus/kebab_mainline` (branch `main`)
- **RIL Repo**: `/serverhive5/zenzer0s/los/hardware/mainline/kona-ril` (branch `empty`, HEAD `8d962cc`)
- **Kernel**: `/serverhive5/zenzer0s/mainline-kona` (branch `android17-6.18-lts-8t`, ACK `android17-6.18-lts`, HEAD `1734a29f39c9`)
- **Reference Trees**:
  - Stock kebab kernel/DTS: `/serverhive5/zenzer0s/ref-stock-kebab`
  - Mainline reference: `/serverhive5/zenzer0s/mainline-kona-ref`
  - Stock vendor dump: `/serverhive5/zenzer0s/stock-vendor`
- **Live ADB Command**:
  ```bash
  export ADB_SERVER_SOCKET=tcp:127.0.0.1:5052
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

## 3. Committed Locally & Ready to Build/Flash Next (NOT Yet Flashed)

Two `vendor.img`-only changes are pending flash (`8d962cc` + `66b6135` in `hardware/mainline/kona-ril`, plus `vendor.prop` in `device/oneplus/kebab_mainline`):

1. **`hardware/mainline/kona-ril` (`8d962cc`) — Fix real IMEI/IMEI SV (`17`) & full Baseband version**:
   - Removed Cuttlefish's hardcoded fake `RefRadioModem::getImei()` override (`"867400022047199"` / `"01"`), so `RadioModem::getImei()` delegates directly to our real QMI DMS `requestDeviceIdentity()` (real SDX55 IMEI + IMEI SV **`17`**).
   - Fixed `qmiDmsGetBasebandVersion()` to trim trailing `.build` suffix after `PACK-<n>` when `> 45` chars so Android `GsmCdmaPhone` (`PROP_VALUE_MAX / 2 = 45`) stops front-truncating `"MPSS.HI.2"` — now displays full **`MPSS.HI.2.5-01066-SDX55_RMTEFS_PACK-1`**.
2. **`hardware/mainline/kona-ril` (`66b6135`) + `device/oneplus/kebab_mainline` (`vendor.prop`) — Enable DSDS (`slot1` + `slot2`) with real dual IMEI/SV & dual UIM slot status**:
   - Enabled `-DSIM_COUNT=2 -DANDROID_MULTI_SIM` across `libcuttlefish-rild`, `libril-modem-lib`, and `libcuttlefish-ril-2`; declared `slot2` VINTF instances for all 8 per-slot radio HALs in `radio-kona.xml` and set `persist.radio.multisim.config=dsds` in `vendor.prop`.
   - Fixed 4 upstream Cuttlefish `SIM_COUNT >= 2` bugs in `ril_service.cpp` (`osAppIdVec` undeclared, duplicate `IImsMedia/default` registration in loop, duplicate `compat::RadioModem`/`compat::RadioSim` publication, off-by-one `getRadioServiceRwlock(slotId)`).
   - Implemented `qmiDmsGetSerialNumbersForSlot()` (`QMI_DMS_BIND_SUBSCRIPTION 0x0054` primary=1 / secondary=2 + `QMI_DMS_GET_DEVICE_SERIAL_NUMBERS 0x0025` over a single QRTR socket → real IMEI 1 + IMEI 2 + IMEI SV `17`), `qmiUimGetSlotsStatus()` (`QMI_UIM_GET_SLOTS_STATUS 0x0047` TLV `0x10` for both physical slots), and an AT-free `onRequestSlot2()` handler so slot 1 5G/LTE/IMS stays completely isolated.
   - Compile-verified all 11 `kona-ril` translation units cleanly with `-DSIM_COUNT=2 -DANDROID_MULTI_SIM`.

**Next Build Command for User (`vendor.img` only!)**:
```bash
m vendorimage
```
(then flash `vendor.img`).

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
