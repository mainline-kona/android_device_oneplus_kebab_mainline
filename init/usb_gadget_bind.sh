#!/system/bin/sh
# Dynamic USB UDC Auto-Binder for OnePlus 8T Recovery
echo "[usb_bind] Starting USB UDC discovery service..." > /dev/kmsg

# 1. Discover UDC
UDC_NAME=""
for i in $(seq 1 30); do
    for u in /sys/class/udc/*; do
        if [ -e "$u" ]; then
            UDC_NAME=$(basename "$u")
            echo "[usb_bind] Discovered UDC: $UDC_NAME" > /dev/kmsg
            break 2
        fi
    done
    sleep 0.2
done

if [ -z "$UDC_NAME" ]; then
    UDC_NAME="a600000.dwc3"
    echo "[usb_bind] Defaulting UDC to $UDC_NAME" > /dev/kmsg
fi

setprop sys.usb.controller "$UDC_NAME"
setprop vendor.usb.controller "$UDC_NAME"

# 2. Ensure configfs is mounted and gadget directories exist
[ ! -d /config/usb_gadget ] && mount -t configfs none /config 2>/dev/null
if [ ! -d /config/usb_gadget/g1 ]; then
    mkdir -p /config/usb_gadget/g1
    echo 0x18d1 > /config/usb_gadget/g1/idVendor
    echo 0xd001 > /config/usb_gadget/g1/idProduct
    mkdir -p /config/usb_gadget/g1/strings/0x409
    echo "OnePlus" > /config/usb_gadget/g1/strings/0x409/manufacturer
    echo "OnePlus 8T" > /config/usb_gadget/g1/strings/0x409/product
    echo "0123456789ABCDEF" > /config/usb_gadget/g1/strings/0x409/serialnumber
    mkdir -p /config/usb_gadget/g1/configs/b.1/strings/0x409
    echo "adb" > /config/usb_gadget/g1/configs/b.1/strings/0x409/configuration
    mkdir -p /config/usb_gadget/g1/functions/ffs.adb
fi

# Ensure functionfs adb is mounted
mkdir -p /dev/usb-ffs/adb
mount -t functionfs adb /dev/usb-ffs/adb 2>/dev/null

# Link ffs.adb to config b.1 if not present
[ ! -e /config/usb_gadget/g1/configs/b.1/f1 ] && ln -sf /config/usb_gadget/g1/functions/ffs.adb /config/usb_gadget/g1/configs/b.1/f1 2>/dev/null

# 3. Ensure adbd is running to handle FunctionFS descriptors
setprop sys.usb.configfs 1
setprop sys.usb.config adb
start adbd

# 4. Wait for FunctionFS to be initialized by adbd
for i in $(seq 1 40); do
    READY=$(getprop sys.usb.ffs.ready)
    if [ "$READY" = "1" ]; then
        echo "[usb_bind] FunctionFS is ready (sys.usb.ffs.ready=1)" > /dev/kmsg
        break
    fi
    sleep 0.25
done

# 5. Bind UDC
CURRENT_UDC=$(cat /config/usb_gadget/g1/UDC 2>/dev/null)
if [ "$CURRENT_UDC" != "$UDC_NAME" ]; then
    echo "" > /config/usb_gadget/g1/UDC 2>/dev/null
    sleep 0.1
    if echo "$UDC_NAME" > /config/usb_gadget/g1/UDC 2>/dev/null; then
        echo "[usb_bind] Successfully bound $UDC_NAME to /config/usb_gadget/g1/UDC" > /dev/kmsg
    else
        echo "[usb_bind] WARNING: Direct echo to UDC failed, retrying..." > /dev/kmsg
        sleep 0.5
        echo "$UDC_NAME" > /config/usb_gadget/g1/UDC 2>/dev/null
    fi
else
    echo "[usb_bind] UDC $UDC_NAME is already bound" > /dev/kmsg
fi

setprop sys.usb.state adb
echo "[usb_bind] Gadget status: UDC=$(cat /config/usb_gadget/g1/UDC 2>/dev/null), state=$(getprop sys.usb.state)" > /dev/kmsg
exit 0
