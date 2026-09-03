#!/bin/bash
# Comprehensive cgroup/zram/zswap exploration for Render web service
# Collects all info and runs active tests, outputs structured report

REPORT=/app/report.txt
exec > >(tee "$REPORT") 2>&1

section() { echo; echo "======================================================================"; echo "  $1"; echo "======================================================================"; }

run() { echo "\$ $*"; eval "$@" 2>&1 | head -100; echo; }
runfull() { echo "\$ $*"; eval "$@" 2>&1; echo; }

echo "=== RENDER CGROUP/ZRAM/ZSWAP EXPLORATION REPORT ==="
echo "Generated: $(date -u 2>/dev/null || echo 'date unavailable')"
echo "Hostname: $(hostname 2>/dev/null)"

section "1. SYSTEM BASICS"
run "uname -a"
run "cat /proc/version"
run "cat /etc/os-release"
run "id"
run "whoami"
run "nproc"
run "cat /proc/uptime"
run "cat /proc/cmdline"

section "2. MEMORY INFO"
run "cat /proc/meminfo"
run "free -b"
run "free -h"

section "3. FILESYSTEMS (cgroup support)"
run "cat /proc/filesystems"

section "4. CGROUP - VERSION & MOUNTS"
run "cat /proc/self/cgroup"
run "cat /proc/1/cgroup"
run "cat /proc/cgroups"
echo "--- mountinfo (cgroup only) ---"
grep -i cgroup /proc/self/mountinfo 2>/dev/null || echo "(no cgroup in mountinfo)"
echo "--- mount (cgroup only) ---"
mount 2>/dev/null | grep -i cgroup || echo "(mount command unavailable or no cgroup mounts)"
echo "--- /sys/fs/cgroup listing ---"
ls -la /sys/fs/cgroup/ 2>&1 | head -40

section "5. CGROUP v2 DETAILS"
run "cat /sys/fs/cgroup/cgroup.controllers"
run "cat /sys/fs/cgroup/cgroup.subtree_control"
run "cat /sys/fs/cgroup/cgroup.stat"
echo "--- Memory controller ---"
run "cat /sys/fs/cgroup/memory.max"
run "cat /sys/fs/cgroup/memory.current"
run "cat /sys/fs/cgroup/memory.swap.max"
run "cat /sys/fs/cgroup/memory.swap.current"
run "cat /sys/fs/cgroup/memory.zswap.max 2>/dev/null || echo 'no zswap.max'"
run "cat /sys/fs/cgroup/memory.stat 2>/dev/null | head -30"
echo "--- CPU controller ---"
run "cat /sys/fs/cgroup/cpu.max"
run "cat /sys/fs/cgroup/cpu.stat"
run "cat /sys/fs/cgroup/cpu.weight"
run "cat /sys/fs/cgroup/cpu.max.burst 2>/dev/null || echo 'no cpu.max.burst'"
echo "--- PIDs controller ---"
run "cat /sys/fs/cgroup/pids.max"
run "cat /sys/fs/cgroup/pids.current"
echo "--- IO controller ---"
run "cat /sys/fs/cgroup/io.max 2>/dev/null || echo 'no io.max'"
run "cat /sys/fs/cgroup/io.stat 2>/dev/null | head -10 || echo 'no io.stat'"

section "6. CGROUP v1 DETAILS (if applicable)"
echo "--- /sys/fs/cgroup/memory/ ---"
ls -la /sys/fs/cgroup/memory/ 2>&1 | head -20
run "cat /sys/fs/cgroup/memory/memory.limit_in_bytes 2>/dev/null || echo 'no v1 memory'"
run "cat /sys/fs/cgroup/memory/memory.usage_in_bytes 2>/dev/null || echo 'no v1 memory'"
run "cat /sys/fs/cgroup/memory/memory.memsw.limit_in_bytes 2>/dev/null || echo 'no v1 memsw'"
run "cat /sys/fs/cgroup/cpu/cpu.cfs_quota_us 2>/dev/null || echo 'no v1 cpu'"
run "cat /sys/fs/cgroup/cpu/cpu.cfs_period_us 2>/dev/null || echo 'no v1 cpu'"

section "7. CGROUP WRITABILITY TEST"
echo "Testing if we can write to cgroup files..."
echo "--- Test: write to memory.max ---"
( echo "536870912" > /sys/fs/cgroup/memory.max 2>&1 && echo "SUCCESS: wrote to memory.max" ) || echo "FAIL: cannot write to memory.max"
echo "--- Test: write to cgroup.subtree_control ---"
( echo "+memory" > /sys/fs/cgroup/cgroup.subtree_control 2>&1 && echo "SUCCESS: wrote to subtree_control" ) || echo "FAIL: cannot write to subtree_control"
echo "--- Test: create sub-cgroup ---"
( mkdir /sys/fs/cgroup/test_subgroup 2>&1 && echo "SUCCESS: created sub-cgroup" && rmdir /sys/fs/cgroup/test_subgroup 2>/dev/null ) || echo "FAIL: cannot create sub-cgroup"

section "8. ZRAM - DEVICES & MODULE"
echo "--- /dev/zram* ---"
ls -la /dev/zram* 2>&1 || echo "(no /dev/zram devices)"
echo "--- /sys/block/ zram ---"
ls /sys/block/ 2>/dev/null | grep zram || echo "(no zram in /sys/block)"
echo "--- lsmod (zram) ---"
lsmod 2>/dev/null | grep -i zram || echo "(zram module not loaded per lsmod)"
echo "--- /proc/modules (zram) ---"
grep -i zram /proc/modules 2>/dev/null || echo "(zram not in /proc/modules)"
echo "--- zramctl ---"
zramctl 2>&1 || echo "(zramctl unavailable or no zram)"
echo "--- /sys/module/zram ---"
ls -la /sys/module/zram/ 2>&1 || echo "(no /sys/module/zram)"

section "9. ZRAM ACTIVE TESTS"
echo "--- Test: modprobe zram ---"
modprobe zram 2>&1 && echo "SUCCESS: modprobe zram worked" || echo "FAIL: modprobe zram failed"
echo "--- Test: insmod (check if we can load modules at all) ---"
modprobe loop 2>&1 && echo "SUCCESS: modprobe loop worked" || echo "FAIL: modprobe loop failed"
echo "--- Test: create zram device via /sys/class/zram-control ---"
ls -la /sys/class/zram-control/ 2>&1 || echo "(no zram-control)"
( echo 1 > /sys/class/zram-control/hot_add 2>&1 && echo "SUCCESS: hot_add worked" ) || echo "FAIL: cannot hot_add zram"
echo "--- Test: zramctl --create ---"
zramctl --find --size 64M 2>&1 && echo "SUCCESS: zramctl create worked" || echo "FAIL: zramctl create failed"
echo "--- Re-check /dev/zram* after tests ---"
ls -la /dev/zram* 2>&1 || echo "(still no /dev/zram devices)"

section "10. ZSWAP - STATUS & PARAMETERS"
echo "--- /sys/module/zswap existence ---"
ls -la /sys/module/zswap/ 2>&1 || echo "(no /sys/module/zswap - zswap not loaded)"
echo "--- zswap parameters ---"
run "cat /sys/module/zswap/parameters/enabled 2>/dev/null || echo 'no zswap enabled param'"
run "cat /sys/module/zswap/parameters/zpool 2>/dev/null || echo 'no zswap zpool param'"
run "cat /sys/module/zswap/parameters/compressor 2>/dev/null || echo 'no zswap compressor param'"
run "cat /sys/module/zswap/parameters/max_pool_percent 2>/dev/null || echo 'no zswap max_pool_percent'"
run "cat /sys/module/zswap/parameters/accept_threshold_percent 2>/dev/null || echo 'no zswap accept_threshold'"
run "cat /sys/module/zswap/parameters/same_filled_pages_enabled 2>/dev/null || echo 'no same_filled_pages'"
run "cat /sys/module/zswap/parameters/non_same_filled_pages_enabled 2>/dev/null || echo 'no non_same_filled_pages'"
echo "--- zswap via /sys/kernel/debug ---"
run "cat /sys/kernel/debug/zswap/pool_total_size 2>/dev/null || echo 'no zswap debug'"
run "cat /sys/kernel/debug/zswap/stored_pages 2>/dev/null || echo 'no zswap debug'"

section "11. ZSWAP ACTIVE TESTS"
echo "--- Test: enable zswap ---"
( echo Y > /sys/module/zswap/parameters/enabled 2>&1 && echo "SUCCESS: enabled zswap" ) || echo "FAIL: cannot write to zswap enabled"
echo "--- Test: disable zswap ---"
( echo N > /sys/module/zswap/parameters/enabled 2>&1 && echo "SUCCESS: disabled zswap" ) || echo "FAIL: cannot write to zswap enabled"
echo "--- Test: change zswap compressor ---"
( echo "lzo" > /sys/module/zswap/parameters/compressor 2>&1 && echo "SUCCESS: changed compressor" ) || echo "FAIL: cannot change compressor"
echo "--- Test: change max_pool_percent ---"
( echo "25" > /sys/module/zswap/parameters/max_pool_percent 2>&1 && echo "SUCCESS: changed max_pool_percent" ) || echo "FAIL: cannot change max_pool_percent"
echo "--- Re-read zswap enabled after tests ---"
cat /sys/module/zswap/parameters/enabled 2>/dev/null || echo "(no zswap enabled param)"

section "12. SWAP - CURRENT STATUS"
run "cat /proc/swaps"
run "swapon --show 2>/dev/null || echo 'swapon --show unavailable'"
run "cat /proc/sys/vm/swappiness"
run "sysctl vm.swappiness 2>/dev/null || echo 'sysctl unavailable'"

section "13. SWAP ACTIVE TESTS"
echo "--- Test: create swap file ---"
dd if=/dev/zero of=/tmp/swapfile bs=1M count=32 2>&1
chmod 600 /tmp/swapfile 2>&1
mkswap /tmp/swapfile 2>&1 && echo "SUCCESS: mkswap worked" || echo "FAIL: mkswap failed"
swapon /tmp/swapfile 2>&1 && echo "SUCCESS: swapon worked" || echo "FAIL: swapon failed"
echo "--- /proc/swaps after test ---"
cat /proc/swaps 2>&1
swapon --show 2>&1 || true
echo "--- Cleanup ---"
swapoff /tmp/swapfile 2>&1 || true
rm -f /tmp/swapfile 2>&1 || true

section "14. CAPABILITIES"
run "cat /proc/self/status | grep -i cap"
run "cat /proc/1/status | grep -i cap"
echo "--- Decoded capabilities (if capsh available) ---"
capsh --decode=$(grep CapEff /proc/self/status | awk '{print $2}') 2>/dev/null || echo "(capsh unavailable)"
capsh --print 2>/dev/null || echo "(capsh unavailable)"

section "15. SYSCTL / VM PARAMETERS"
run "cat /proc/sys/vm/swappiness"
run "cat /proc/sys/vm/overcommit_memory"
run "cat /proc/sys/vm/overcommit_ratio"
run "cat /proc/sys/vm/watermark_scale_factor 2>/dev/null || echo 'n/a'"
run "cat /proc/sys/vm/min_free_kbytes"
run "cat /proc/sys/kernel/hostname"
echo "--- Test: write sysctl ---"
( echo 60 > /proc/sys/vm/swappiness 2>&1 && echo "SUCCESS: wrote swappiness" ) || echo "FAIL: cannot write swappiness"
echo "--- sysctl -a (vm/kernel only) ---"
sysctl -a 2>/dev/null | grep -E "^(vm|kernel)\." | head -50 || echo "(sysctl -a unavailable)"

section "16. DEVICES & BLOCK LAYER"
run "ls -la /dev/ | head -50"
echo "--- Block devices ---"
run "lsblk 2>/dev/null || echo 'lsblk unavailable'"
echo "--- /sys/block/ ---"
ls /sys/block/ 2>/dev/null || echo "(no /sys/block)"

section "17. NAMESPACE & ISOLATION"
run "cat /proc/self/ns/cgroup 2>/dev/null || echo 'no ns cgroup'"
run "ls -la /proc/self/ns/ 2>/dev/null || echo 'no ns dir'"
run "cat /proc/self/uid_map 2>/dev/null || echo 'no uid_map'"
run "cat /proc/self/gid_map 2>/dev/null || echo 'no gid_map'"

section "18. SECCOMP / SECURITY"
run "cat /proc/self/status | grep -i seccomp"
run "cat /proc/1/status | grep -i seccomp"

section "19. HOST KERNEL MODULES (full list)"
runfull "cat /proc/modules | head -50"
echo "Total loaded modules: $(wc -l < /proc/modules 2>/dev/null || echo 0)"

section "20. DETAILED MOUNT INFO"
runfull "cat /proc/self/mountinfo | head -60"

section "21. /proc/self/maps (first 20 lines)"
run "cat /proc/self/maps | head -20"

section "22. CGROUP v2 MEMORY PRESSURE & EVENTS"
run "cat /sys/fs/cgroup/memory.pressure 2>/dev/null || echo 'no memory.pressure'"
run "cat /sys/fs/cgroup/memory.events 2>/dev/null || echo 'no memory.events'"
run "cat /sys/fs/cgroup/memory.events.local 2>/dev/null || echo 'no memory.events.local'"
run "cat /sys/fs/cgroup/memory.peak 2>/dev/null || echo 'no memory.peak'"
run "cat /sys/fs/cgroup/memory.oom.group 2>/dev/null || echo 'no memory.oom.group'"

section "23. CGROUP v2 MISC CONTROLLERS"
run "cat /sys/fs/cgroup/rdma.max 2>/dev/null || echo 'no rdma.max'"
run "cat /sys/fs/cgroup/hugetlb.1GB.max 2>/dev/null || echo 'no hugetlb'"
run "cat /sys/fs/cgroup/misc.current 2>/dev/null || echo 'no misc.current'"
run "cat /sys/fs/cgroup/misc.max 2>/dev/null || echo 'no misc.max'"

section "24. KSM (Kernel Samepage Merging)"
run "cat /sys/kernel/mm/ksm/run 2>/dev/null || echo 'no ksm'"
run "cat /sys/kernel/mm/ksm/max_page_sharing 2>/dev/null || echo 'no ksm'"

section "25. TRANSPARENT HUGE PAGES"
run "cat /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || echo 'no thp'"
run "cat /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || echo 'no thp defrag'"

section "26. BOOT PARAMS / KERNEL FEATURES"
run "cat /sys/kernel/kexec_loaded 2>/dev/null || echo 'n/a'"
run "cat /proc/sys/kernel/random/boot_id 2>/dev/null || echo 'n/a'"

section "27. CGROUP FREEZER & KILL TEST"
run "cat /sys/fs/cgroup/cgroup.freeze 2>/dev/null || echo 'no freeze'"
run "cat /sys/fs/cgroup/cgroup.events 2>/dev/null || echo 'no events'"

section "28. FULL CGROUP TREE (depth 2)"
find /sys/fs/cgroup -maxdepth 2 -type f 2>/dev/null | head -80 || echo "(cannot traverse cgroup tree)"
echo "--- cgroup dirs ---"
find /sys/fs/cgroup -maxdepth 2 -type d 2>/dev/null | head -30 || echo "(cannot traverse cgroup dirs)"

section "29. ZRAM DETAILED (if devices exist)"
for dev in /dev/zram*; do
    [ -e "$dev" ] || continue
    base=$(basename "$dev")
    echo "=== $dev ==="
    run "cat /sys/block/$base/comp_algorithm 2>/dev/null || echo 'n/a'"
    run "cat /sys/block/$base/disksize 2>/dev/null || echo 'n/a'"
    run "cat /sys/block/$base/initstate 2>/dev/null || echo 'n/a'"
    run "cat /sys/block/$base/reset 2>/dev/null || echo 'n/a'"
    run "cat /sys/block/$base/mem_used_total 2>/dev/null || echo 'n/a'"
done

section "30. SUMMARY OF ACTIVE TESTS"
echo "Collecting test results summary..."
echo "zram_module_loaded: $(lsmod 2>/dev/null | grep -c zram)"
echo "zram_devices: $(ls /dev/zram* 2>/dev/null | wc -l)"
echo "zswap_enabled: $(cat /sys/module/zswap/parameters/enabled 2>/dev/null || echo 'N/A')"
echo "zswap_exists: $(ls -d /sys/module/zswap 2>/dev/null && echo yes || echo no)"
echo "cgroup_v2: $(grep -c cgroup2 /proc/self/mountinfo 2>/dev/null || echo 0)"
echo "cgroup_v1: $(grep -c 'cgroup ' /proc/self/mountinfo 2>/dev/null || echo 0)"
echo "swap_count: $(grep -c . /proc/swaps 2>/dev/null || echo 0)"
echo "is_root: $(id -u 2>/dev/null)"
echo "can_write_sysfs: $(test -w /sys/module/ 2>/dev/null && echo yes || echo no)"

echo
echo "======================================================================"
echo "  END OF REPORT"
echo "======================================================================"
