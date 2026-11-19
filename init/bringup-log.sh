#!/vendor/bin/sh
#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

# /metadata is small and the boot needs it, so keep only the last few boots and cap each log
d=/metadata/bringup
mkdir -p $d

case "$1" in
    kmsg)
        n=$(($(cat $d/count 2>/dev/null || echo 0) + 1))
        echo $n > $d/count
        # By boot number: the clock is not set yet, so mtimes jump between boots
        for f in $d/kmsg-* $d/logcat-*; do
            b=${f##*-}
            b=${b%%.*}
            [ "$b" -lt $((n - 2)) ] 2>/dev/null && rm -f $f
        done
        cat /dev/kmsg | head -c 1048576 > $d/kmsg-$n.txt
        ;;
    logcat)
        sleep 1
        n=$(cat $d/count 2>/dev/null || echo 0)
        exec /system/bin/logcat -b all -v threadtime -r 512 -n 1 -f $d/logcat-$n.txt
        ;;
esac
