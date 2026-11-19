#!/system/bin/sh
# kebab-accept.sh — on-device acceptance checks for the kebab mainline stack.
# Run over adb (owner):  adb -H <host> -P <port> shell sh /data/local/tmp/kebab-accept.sh
# Read-only: it only queries properties, logs and interfaces.

PASS=0; FAIL=0

chk() {
    if eval "$2"; then
        echo "PASS $1"; PASS=$((PASS+1))
    else
        echo "FAIL $1"; FAIL=$((FAIL+1))
    fi
}

prop_is() { [ "$(getprop "$1")" = "$2" ]; }
prop_nonempty() { [ -n "$(getprop "$1")" ]; }

echo "== implementation"
prop_is persist.vendor.kebab.ril_impl qmi \
    && echo "note: qmi daemon active" || echo "note: reference-ril active (ref)"

echo "== SIM / identity"
chk "sim state READY|LOADED" \
    "case \"\$(getprop gsm.sim.state)\" in READY*|LOADED*) true;; *) false;; esac"
chk "iccid present" "prop_nonempty gsm.sim.operator.numeric"
chk "operator numeric" "case \"\$(getprop gsm.operator.numeric)\" in ?????) true;; *) false;; esac"
chk "network type not unknown" \
    "case \"\$(getprop gsm.network.type)\" in UNKNOWN|'') false;; *) true;; esac"

echo "== data"
chk "mhi_hwip0 exists" "ip link show mhi_hwip0 >/dev/null 2>&1"
chk "mhi_hwip0 has v4 address" \
    "ip -4 addr show mhi_hwip0 2>/dev/null | grep -q 'inet '"
chk "ping 8.8.8.8" "ping -c 1 -W 3 8.8.8.8 >/dev/null 2>&1"

echo "== telephony services"
chk "ril daemon running" \
    "getprop init.svc.vendor.ril-daemon | grep -q running || getprop init.svc.vendor.kebab_rild | grep -q running"
chk "IMS service present" "dumpsys ims 2>/dev/null | grep -qi ims"

echo "== kernel / modem"
chk "voice mailbox configured" \
    "dmesg 2>/dev/null | grep -q 'voice mailbox configured' || logcat -d -b kernel 2>/dev/null | grep -q 'voice mailbox configured'"
chk "CS-VOICE PCM present" "grep -qr CS-VOICE /proc/asound 2>/dev/null"
chk "single voicecall helper" \
    "[ \"\$(ps -A 2>/dev/null | grep -c '[k]ebab-voicecall')\" -le 1 ]"
chk "no AT timeouts in logcat" \
    "! logcat -d 2>/dev/null | grep -q 'AT timeout'"

echo "== daemon log (when ril_impl=qmi)"
logcat -d -s kebab-rild 2>/dev/null | tail -20

echo "== result: PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ]
