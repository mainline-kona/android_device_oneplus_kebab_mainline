#!/system/bin/sh
# Dynamic USB UDC Auto-Binder for OnePlus 8T Recovery
echo "[usb_bind] Starting USB UDC discovery service..." > /dev/kmsg

# Ensure configfs is mounted
[ ! -d /config/usb_gadget ] && mount -t configfs none /config 2>/dev/null

# Prepare USB Gadget g1 if not created by init
if [ ! -d /config/usb_gadget/g1 ]; then
    mkdir -p /config/usb_gadget/g1
    echo 0x18d1 > /config/usb_gadget/g1/idVendor
    echo 0x4ee7 > /config/usb_gadget/g1/idProduct
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

# Link ffs.adb to config b.1
[ ! -e /config/usb_gadget/g1/configs/b.1/f1 ] && ln -sf /config/usb_gadget/g1/functions/ffs.adb /config/usb_gadget/g1/configs/b.1/f1 2>/dev/null

for i in $(seq 1 30); do
    for u in /sys/class/udc/*; do
        if [ -e "$u" ]; then
            UDC_NAME=$(basename "$u")
            echo "[usb_bind] Found UDC: $UDC_NAME" > /dev/kmsg
            setprop sys.usb.controller "$UDC_NAME"
            setprop vendor.usb.controller "$UDC_NAME"
            
            # Unbind first if bound to avoid resource busy error
            echo "" > /config/usb_gadget/g1/UDC 2>/dev/null
            sleep 0.1
            echo "$UDC_NAME" > /config/usb_gadget/g1/UDC 2>/dev/null
            echo "[usb_bind] Successfully bound $UDC_NAME to /config/usb_gadget/g1/UDC" > /dev/kmsg
            
            setprop sys.usb.state adb
            setprop sys.usb.config adb
            start adbd
            exit 0
        fi
    done
    sleep 0.5
done

echo "[usb_bind] Warning: No UDC device detected within 15 seconds" > /dev/kmsg
exit 1
