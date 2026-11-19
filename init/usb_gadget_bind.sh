#!/system/bin/sh
# Dynamic USB UDC Auto-Binder for OnePlus 8T Recovery
echo "[usb_bind] Starting USB UDC discovery service..." > /dev/kmsg

for i in $(seq 1 30); do
    for u in /sys/class/udc/*; do
        if [ -e "$u" ]; then
            UDC_NAME=$(basename "$u")
            echo "[usb_bind] Found UDC: $UDC_NAME" > /dev/kmsg
            setprop sys.usb.controller "$UDC_NAME"
            setprop vendor.usb.controller "$UDC_NAME"
            
            if [ -f /config/usb_gadget/g1/UDC ]; then
                CURRENT_UDC=$(cat /config/usb_gadget/g1/UDC 2>/dev/null)
                if [ "$CURRENT_UDC" != "$UDC_NAME" ]; then
                    echo "$UDC_NAME" > /config/usb_gadget/g1/UDC 2>/dev/null
                    echo "[usb_bind] Successfully bound $UDC_NAME to ConfigFS gadget!" > /dev/kmsg
                fi
            fi
            setprop sys.usb.state adb
            exit 0
        fi
    done
    sleep 0.5
done

echo "[usb_bind] Warning: No UDC device detected within 15 seconds" > /dev/kmsg
exit 1
