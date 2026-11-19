#!/system/bin/sh
# Dynamic USB UDC Auto-Binder for OnePlus 8T Recovery
echo "[usb_bind] Starting USB UDC discovery service..." > /dev/kmsg

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
    UDC_NAME="a600000.usb"
    echo "[usb_bind] Defaulting UDC to $UDC_NAME" > /dev/kmsg
fi

setprop sys.usb.controller "$UDC_NAME"
setprop vendor.usb.controller "$UDC_NAME"

echo "[usb_bind] USB controller set to $UDC_NAME" > /dev/kmsg
exit 0
