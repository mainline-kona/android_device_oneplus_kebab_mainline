# OnePlus 8T (kebab) Mainline Linux Device Tree

This repository provides the mainline Linux device tree for building and booting AOSP / PenguinOS on the OnePlus 8T (`kebab`, SM8250).

## Hardware & Architecture
- **SoC**: Qualcomm Snapdragon 865 (SM8250)
- **Target Device**: OnePlus 8T (`kebab` / `KB2005`)
- **Kernel**: Mainline Linux 7.2.0 (`7.2.0-android`)
- **Graphics**: Mesa 3D Freedreno (Turnip Vulkan + Gallium GLES)
- **Composer**: `drm_hwcomposer-upstream`
- **Memory / Gralloc**: `minigbm-upstream`
- **Audio**: ALSA UCM2 via kernel ASoC LPASS
- **Security**: AIDL Software Gatekeeper & KeyMint (optional stock QTEE bridge)

## Partition Layout & Boot Specifications
- **Boot Header Version**: 2
- **AVB Flags**: `--flags 3` (avoids dm-verity enforcement halts during bringup)
- **Boot Partition Size**: `100,663,296` bytes (96.00 MiB exact)
- **Recovery Partition Size**: `100,663,296` bytes (96.00 MiB exact)
- **Super Dynamic Partitions Size**: `7,516,192,768` bytes

## Build Instructions
```bash
source build/envsetup.sh
lunch custom_kebab_mainline-trunk_staging-userdebug
m bootimage recoveryimage
```
