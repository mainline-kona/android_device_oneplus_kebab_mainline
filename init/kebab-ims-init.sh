#!/vendor/bin/sh
# IMS bring-up for the SDX55: wait for the modem to be online, register the
# IMSA/IMSS clients and their indications, program the IMS APN and WMS
# domain preference. Mirrors what stock qcrild does at boot.
# init runs this with stdio_to_kmsg, so every line lands in the kernel log.
kdc=/vendor/bin/kebab-dataconn

log() { echo "kebab-ims-init: $*"; }

# wait up to ~120 s for DMS operating mode online (0x2d, TLV01 == 0)
log "waiting for modem online"
i=0
online=0
while [ $i -lt 60 ]; do
    if $kdc --qmi 0x02 0x002d 2>&1 | grep -q "tlv 0x01 len 1: 00"; then
        online=1
        break
    fi
    sleep 2
    i=$((i + 1))
done
[ $online = 1 ] || log "modem did not report online within 120 s"

# IMSA: bind (u32 TLV) then enable registration/service indications
log "IMSA bind + indications"
$kdc --qmi 0x21 0x0033 10:00000000 + 0x0022 10:01 11:01

# IMSS: bind (u32 TLV) then enable VoLTE / VT / IMS service / UT
log "IMSS bind + indications"
$kdc --qmi 0x12 0x0098 01:00000000 + 0x008f 10:01 11:01 18:01 19:01

# DSD APN map: ims (type 1) + default (type 0), chained on one socket
log "DSD APN map (ims + default)"
$kdc --qmi 0x2a 0x0032 01:0100000003696d73 10:00 + 0x0032 01:00000000066a696f6e6574 10:00

# WMS: lte_domain_pref = IMS, so SMS uses the IMS transport
log "WMS IMS domain preference"
$kdc --qmi 0x05 0x0052 10:01

log "done"
exit 0
