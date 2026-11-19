#!/system/bin/sh
# Multi-Path Remoteproc DSP Firmware Mount, Symlinking & Auto-Boot Script for OnePlus 8T
echo "[fw_mount] Starting DSP firmware partition mounting & setup..." > /dev/kmsg

mkdir -p /vendor/firmware_mnt /vendor/dsp /lib/firmware/qcom/sm8250/OnePlus/kebab
mkdir -p /dev/block

# Ensure bootdevice and by-name symlinks exist
if [ -d /dev/block/platform/soc@0/1d84000.ufshc ]; then
    [ ! -e /dev/block/bootdevice ] && ln -sf /dev/block/platform/soc@0/1d84000.ufshc /dev/block/bootdevice 2>/dev/null
    [ ! -e /dev/block/by-name ] && ln -sf /dev/block/platform/soc@0/1d84000.ufshc/by-name /dev/block/by-name 2>/dev/null
elif [ -d /dev/block/platform/soc/1d84000.ufshc ]; then
    [ ! -e /dev/block/bootdevice ] && ln -sf /dev/block/platform/soc/1d84000.ufshc /dev/block/bootdevice 2>/dev/null
    [ ! -e /dev/block/by-name ] && ln -sf /dev/block/platform/soc/1d84000.ufshc/by-name /dev/block/by-name 2>/dev/null
fi

find_part() {
    BASE="$1"
    for s in "_b" "_a" ""; do
        for p in "/dev/block/by-name/${BASE}${s}"                  "/dev/block/bootdevice/by-name/${BASE}${s}"                  "/dev/block/platform/soc@0/1d84000.ufshc/by-name/${BASE}${s}"                  "/dev/block/platform/soc/1d84000.ufshc/by-name/${BASE}${s}"; do
            if [ -b "$p" ]; then
                echo "$p"
                return 0
            fi
        done
    done
    return 1
}

# Wait for block devices (up to 15 seconds)
MODEM_BLK=""
DSP_BLK=""
for i in $(seq 1 30); do
    MODEM_BLK=$(find_part "modem")
    DSP_BLK=$(find_part "dsp")
    if [ -n "$MODEM_BLK" ] || [ -n "$DSP_BLK" ]; then
        break
    fi
    sleep 0.5
done

# Mount modem partition (holds ADSP, CDSP, and SLPI firmware)
if [ -n "$MODEM_BLK" ]; then
    echo "[fw_mount] Mounting modem partition from $MODEM_BLK..." > /dev/kmsg
    mount -t vfat -o ro "$MODEM_BLK" /vendor/firmware_mnt 2>/dev/null || mount -o ro "$MODEM_BLK" /vendor/firmware_mnt 2>/dev/null
fi

# Mount dsp partition (holds DSP additional images)
if [ -n "$DSP_BLK" ]; then
    echo "[fw_mount] Mounting dsp partition from $DSP_BLK..." > /dev/kmsg
    mount -t ext4 -o ro "$DSP_BLK" /vendor/dsp 2>/dev/null || mount -o ro "$DSP_BLK" /vendor/dsp 2>/dev/null
fi

TARGET_DIR="/lib/firmware/qcom/sm8250/OnePlus/kebab"

# Copy all OEM firmware split chunks and headers (.mdt, .b00-.b21) into ramdisk target
if [ -d /vendor/firmware_mnt/image ]; then
    echo "[fw_mount] Copying files from /vendor/firmware_mnt/image/ to $TARGET_DIR..." > /dev/kmsg
    cp -r /vendor/firmware_mnt/image/* "$TARGET_DIR/" 2>/dev/null
elif [ -d /vendor/firmware_mnt ]; then
    echo "[fw_mount] Copying files from /vendor/firmware_mnt/ to $TARGET_DIR..." > /dev/kmsg
    cp -r /vendor/firmware_mnt/* "$TARGET_DIR/" 2>/dev/null
fi

if [ -d /vendor/dsp/image ]; then
    echo "[fw_mount] Copying files from /vendor/dsp/image/ to $TARGET_DIR..." > /dev/kmsg
    cp -r /vendor/dsp/image/* "$TARGET_DIR/" 2>/dev/null
elif [ -d /vendor/dsp ]; then
    echo "[fw_mount] Copying files from /vendor/dsp/ to $TARGET_DIR..." > /dev/kmsg
    cp -r /vendor/dsp/* "$TARGET_DIR/" 2>/dev/null
fi

# Create MDT -> MBN symlinks (Verified Working Solution on Live Hardware)
cd "$TARGET_DIR"
[ -f adsp.mdt ] && ln -sf adsp.mdt adsp.mbn
[ -f cdsp.mdt ] && ln -sf cdsp.mdt cdsp.mbn
[ -f slpi.mdt ] && ln -sf slpi.mdt slpi.mbn

# Fallback: If MDT is missing MBN, run pil-squasher if available
if [ ! -e "$TARGET_DIR/adsp.mbn" ] && [ -x /system/bin/pil-squasher ] && [ -f /vendor/firmware_mnt/image/adsp.mdt ]; then
    echo "[fw_mount] Fallback: Squashing adsp.mdt..." > /dev/kmsg
    /system/bin/pil-squasher "$TARGET_DIR/adsp.mbn" /vendor/firmware_mnt/image/adsp.mdt 2>/dev/null
fi
if [ ! -e "$TARGET_DIR/cdsp.mbn" ] && [ -x /system/bin/pil-squasher ] && [ -f /vendor/dsp/image/cdsp.mdt ]; then
    echo "[fw_mount] Fallback: Squashing cdsp.mdt..." > /dev/kmsg
    /system/bin/pil-squasher "$TARGET_DIR/cdsp.mbn" /vendor/dsp/image/cdsp.mdt 2>/dev/null
fi
if [ ! -e "$TARGET_DIR/slpi.mbn" ] && [ -x /system/bin/pil-squasher ] && [ -f /vendor/firmware_mnt/image/slpi.mdt ]; then
    echo "[fw_mount] Fallback: Squashing slpi.mdt..." > /dev/kmsg
    /system/bin/pil-squasher "$TARGET_DIR/slpi.mbn" /vendor/firmware_mnt/image/slpi.mdt 2>/dev/null
fi

# Link all squashed/symlinked MBNs into all system firmware search paths
for extra in /lib/firmware /lib/firmware/qcom /system/lib/firmware/qcom/sm8250/OnePlus/kebab /vendor/firmware/qcom/sm8250/OnePlus/kebab; do
    mkdir -p "$extra"
    for mbn in "$TARGET_DIR"/*.mbn; do
        if [ -e "$mbn" ]; then
            ln -sf "$mbn" "$extra/$(basename "$mbn")" 2>/dev/null
        fi
    done
done

echo "[fw_mount] Firmware directory staged: $(ls -m $TARGET_DIR/*.mbn 2>/dev/null)" > /dev/kmsg
echo "[fw_mount] Starting offline DSP remoteprocs..." > /dev/kmsg

# Dynamic kick of remoteprocs with watchdog loop
for attempt in 1 2 3 4 5; do
    ALL_RUNNING=1
    for rproc in /sys/class/remoteproc/remoteproc*; do
        if [ -e "$rproc/state" ]; then
            STATE=$(cat "$rproc/state" 2>/dev/null)
            NAME=$(cat "$rproc/name" 2>/dev/null)
            if [ "$STATE" != "running" ]; then
                ALL_RUNNING=0
                echo "[fw_mount] Attempt $attempt: Starting $NAME ($rproc, state: $STATE)..." > /dev/kmsg
                echo start > "$rproc/state" 2>/dev/null
                NEW_STATE=$(cat "$rproc/state" 2>/dev/null)
                echo "[fw_mount] $NAME ($rproc) new state: $NEW_STATE" > /dev/kmsg
            else
                echo "[fw_mount] $NAME ($rproc) is RUNNING" > /dev/kmsg
            fi
        fi
    done
    if [ "$ALL_RUNNING" = "1" ]; then
        echo "[fw_mount] SUCCESS: ALL DSP remoteprocs are verified RUNNING!" > /dev/kmsg
        break
    fi
    sleep 1
done

exit 0
