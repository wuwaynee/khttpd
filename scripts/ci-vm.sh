#!/usr/bin/env bash
# Build an isolated initramfs; never load the module into the CI host kernel.
set -euo pipefail
cd "$(dirname "$0")/.."
[[ $# == 1 ]] || { echo "usage: $0 READABLE_KERNEL_IMAGE" >&2; exit 2; }
image=$1
command -v qemu-system-x86_64 >/dev/null
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp"/{bin,dev,proc,sys,tmp}
cp "$(command -v busybox)" "$tmp/bin/busybox"
ln -s busybox "$tmp/bin/sh"
cp khttpd.ko "$tmp/khttpd.ko"
"${CC:-cc}" -static -O2 -std=c11 -Wall -Wextra -Werror -pthread \
    tests/integration.c -o "$tmp/integration"
sudo mknod "$tmp/dev/console" c 5 1
sudo mknod "$tmp/dev/null" c 1 3
cat > "$tmp/init" <<'INIT'
#!/bin/sh
set -eu
export PATH=/bin
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sysfs /sys
ip link set lo up
trap 'echo KH_VM_FAIL; poweroff -f' EXIT
uname -a
for args in "workers=0" "shards=65" "timeout_ms=0" "bind_addr=bad"; do
    if insmod /khttpd.ko $args; then exit 1; fi
    test ! -d /sys/module/khttpd
done
for fail in 0 1 8; do
    if insmod /khttpd.ko workers=8 fail_start_after="$fail"; then exit 1; fi
    test ! -d /sys/module/khttpd
done
for config in "1 1" "2 0" "2 1"; do
    set -- $config
    insmod /khttpd.ko workers=32 shards="$1" prealloc="$2" timeout_ms=2000
    /integration 8081
    /integration 8081 deadline
    cat /sys/module/khttpd/parameters/stats
    /integration 8081 hold > /tmp/hold.log &
    holder=$!
    ready=0
    for i in $(seq 1 100); do
        if grep -q HOLD_READY /tmp/hold.log; then ready=1; break; fi
        sleep 0.05
    done
    test "$ready" = 1
    rmmod khttpd
    wait "$holder"
    cat /tmp/hold.log
    test ! -d /sys/module/khttpd
done
insmod /khttpd.ko workers=4 max_requests=2
/integration 8081 limit
rmmod khttpd
for i in $(seq 1 10); do
    insmod /khttpd.ko workers=4
    rmmod khttpd
done
if dmesg | grep -E 'BUG:|WARNING:|KASAN:|general protection fault|Kernel panic'; then
    exit 1
fi
echo KH_VM_PASS
trap - EXIT
poweroff -f
INIT
chmod +x "$tmp/init"
(cd "$tmp" && find . -print0 | cpio --null -o --format=newc 2>/dev/null) \
    | gzip -1 > build/initramfs.cpio.gz
if ! timeout 180s qemu-system-x86_64 -accel tcg -cpu max -smp 2 -m 768 \
    -display none -serial stdio -monitor none -no-reboot -nic none \
    -kernel "$image" -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=1 oops=panic rdinit=/init" \
    > build/vm.log 2>&1; then
    cat build/vm.log
    exit 1
fi
cat build/vm.log
rg -q '^KH_VM_PASS' build/vm.log
! rg -q '^KH_VM_FAIL' build/vm.log
