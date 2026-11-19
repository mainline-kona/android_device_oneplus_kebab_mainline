#!/vendor/bin/sh
# Gate the DSP (SLPI/ADSP) start on pd-mapper being up: the sensor core needs
# its protection-domain maps published before it initializes.
i=0
while [ $i -lt 100 ]; do
    [ "$(getprop init.svc.vendor.pd-mapper)" = "running" ] && break
    sleep 0.1
    i=$((i + 1))
done
sleep 1
setprop sys.kebab.pdready 1
