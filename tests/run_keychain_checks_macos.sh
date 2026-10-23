#!/bin/bash
# For the Intel iMac without Python/CLT. Binaries are signed on the build Mac.
set -eu
charms_root=$(cd "${1:?Supply the staged keychain-native-checks directory}" && pwd -P)
case "$charms_root" in *keychain-native-checks*) ;; *) exit 64;; esac
charms_fixture="$charms_root/fixture-$(/usr/bin/uuidgen).keychain-db"
charms_service="charms-keychain-test/$(/usr/bin/uuidgen)"
charms_probe="$charms_root/stable-probe"
charms_backup="$charms_root/valid-probe"
cp "$charms_probe" "$charms_backup"
replace_probe() { cp "$1" "$charms_root/replacement-probe"; mv -f "$charms_root/replacement-probe" "$charms_probe"; }
cleanup() {
    replace_probe "$charms_backup"
    if [ -f "$charms_fixture" ]; then /usr/bin/security delete-keychain "$charms_fixture" >/dev/null 2>&1; fi
}
trap cleanup EXIT
call() { "$1" "$2" "$charms_fixture" "$charms_service"; }
reject() {
    charms_started=$(/bin/date +%s)
    if call "$1" "$2"; then echo 'FAIL: unauthorized operation succeeded'; exit 1; else
        [ "$?" = 2 ] || exit 1
    fi
    [ $(($(/bin/date +%s)-charms_started)) -lt 4 ] || exit 1
}
"$charms_probe" --ipc-check
"$charms_probe" --ipc-check-reaped
call "$charms_probe" create
call "$charms_probe" read
call "$charms_probe" read
call "$charms_root/next-probe" read
replace_probe "$charms_root/next-probe"
call "$charms_probe" read
call "$charms_probe" update
call "$charms_probe" read
cp "$charms_probe" "$charms_backup"
call "$charms_probe" lock
reject "$charms_probe" read
call "$charms_probe" unlock
reject "$charms_root/foreign-probe" read
reject "$charms_root/foreign-probe" update
reject "$charms_root/foreign-probe" delete
replace_probe "$charms_root/foreign-probe"
reject "$charms_probe" read
replace_probe "$charms_backup"
call "$charms_probe" read
call "$charms_probe" delete
echo 'PASS: native macOS restarts, changed/moved executable, stable certificate, private pipes, locked Keychain and foreign-signature rejection.'
echo 'PASS: synthetic credentials only; disposable Keychain removed.'
