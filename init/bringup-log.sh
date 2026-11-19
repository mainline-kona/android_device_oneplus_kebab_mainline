#!/vendor/bin/sh
#
# Copyright (C) 2026 The PenguinOS Project
# SPDX-License-Identifier: Apache-2.0
#

# /metadata is only 11 MB and early boot needs free space on it (aconfig
# platform_storage_records.pb failed with ENOSPC once these logs filled it),
# so keep only this boot and the previous one, about 2.8 MB per boot at most.
d=/metadata/bringup
mkdir -p $d

case "$1" in
    kmsg)
        n=$(($(cat $d/count 2>/dev/null || echo 0) + 1))
        echo $n > $d/count
        # By boot number: keep only the current and the previous boot
        for f in $d/kmsg-* $d/logcat-*; do
            b=${f##*-}
            b=${b%%.*}
            [ "$b" -lt $((n - 1)) ] 2>/dev/null && rm -f $f
        done
        cat /dev/kmsg | head -c 1048576 > $d/kmsg-$n.txt
        ;;
    logcat)
        sleep 1
        n=$(cat $d/count 2>/dev/null || echo 0)
        exec /system/bin/logcat -b all -v threadtime -r 384 -n 1 -f $d/logcat-$n.txt
        ;;
    diag)
        # X55 F3 messages from the moment the DIAG port appears, so RF init
        # (rfm_init / rfcommon_debug) at modem boot is captured. QShrink-hashed
        # frames come out as "H .." lines: decode with images/qdb_decode.py.
        n=$(cat $d/count 2>/dev/null || echo 0)
        for f in $d/diag-*; do
            b=${f##*-}
            b=${b%%.*}
            [ "$b" -lt $((n - 1)) ] 2>/dev/null && rm -f $f
        done
        i=0
        while [ ! -e /dev/wwan0qcdm0 ] && [ $i -lt 1200 ]; do
            sleep 0.1
            i=$((i + 1))
        done
        # The node shows up before the MHI channel can be opened: retry
        i=0
        while [ $i -lt 100 ]; do
            /vendor/bin/kebab-dataconn --diag-msgs 150 | head -c 1048576 > $d/diag-$n.txt
            grep -q '^error=open' $d/diag-$n.txt || break
            sleep 0.2
            i=$((i + 1))
        done
        ;;
esac
