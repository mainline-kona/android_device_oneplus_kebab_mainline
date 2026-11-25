#!/vendor/bin/sh
# Gate the DSP (SLPI/ADSP) start on pd-mapper being up: the sensor core needs
# its protection-domain maps published before it initializes.
i=0
while [ $i -lt 100 ]; do
    [ "$(getprop init.svc.vendor.pd-mapper)" = "running" ] && break
    sleep 0.1
    i=$((i + 1))
done
sleep 0.2
# If ath11k_pci hit the switch_root window before /vendor was mounted (m3.bin -ENOENT),
# rebind it now that /vendor/firmware is mounted so wlan0 always comes up.
if [ ! -d /sys/class/net/wlan0 ] && [ -e /sys/bus/pci/drivers/ath11k_pci/0000:01:00.0 ]; then
    echo 0000:01:00.0 > /sys/bus/pci/drivers/ath11k_pci/unbind
    echo 0000:01:00.0 > /sys/bus/pci/drivers/ath11k_pci/bind
fi
# Ensure SLPI's hexagonrpcd-sdsp (uid=system) owns the entire sensor registry
# tree before SLPI boots; root-owned files cause EACCES and a ~60s SLPI stall.
chown -R system:system /mnt/vendor/persist/sensors 2>/dev/null || true
# Boot SLPI/CDSP/ADSP immediately in parallel with post-fs-data/apexd so SLPI
# is up before class_start hal starts vendor.sensors-mainline (otherwise libssc
# hits 6x10s QMI timeouts delaying system_server by ~62s).
/vendor/bin/sh /vendor/bin/init.mainline.qcom.start_remoteproc.sh
setprop sys.kebab.pdready 1
