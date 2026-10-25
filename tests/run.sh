#!/bin/bash
#
# Build the kernel with the host toolchain and run the QEMU test suite.
#
# usage: tests/run.sh [-a arch] [-C cc] [-c cpus] [-j jobs] [-k args] [-n] [-v] [test...]
#
#   -a      the architecture: x86_64 (default), i386, or aarch64 (QEMU's
#           virt machine, the kernel and tests built with clang; the
#           aarch64 and oom tests)
#   -C      the kernel's compiler on x86: gcc (default) or tcc (TCC=<tcc>,
#           tests/host/<arch>-pc-tcc.mk), its build in tests/out/<arch>-tcc
#   -c      the CPUs QEMU has (-smp), every boot (but the irq and smp
#           tests', which choose)
#   -j      tests run at once (default 1); the ones that time things or
#           take much memory (clock, irq, smp, memory) run alone, after
#   -k      more for the kernel's command line, every boot (e.g. apic=off;
#           but the disk test's, Limine's configuration says it)
#   -n      don't rebuild the kernel, use the existing image
#   -v      print the serial log of failed tests
#
# QEMU uses KVM when /dev/kvm can be used, KVM=0 in the environment turns
# it off.
#
# tests: ata cpu syscalls memory oom kvmem clock irq smp net tcp ext2 minix libc system disk tcc curl
# bash; aarch64 and oom (-a aarch64)
# (default: all)
#
# Requires gcc with 32-bit support, ld, cpio and qemu-system-x86_64 (and
# qemu-system-i386 for i386; clang, ld.lld, llvm-objcopy and
# qemu-system-aarch64 for aarch64). The ext2 test also needs e2fsprogs
# (mke2fs, e2fsck, debugfs) and python3. The libc, system and disk tests
# need the cross toolchain (make toolchain, x86_64-aquila or i686-aquila)
# and are skipped without it, the disk test also sfdisk, mkfs.fat, mtools and
# fakeroot (mk/hdd.sh). -C tcc needs tcc (i386: its i386 compiler, i386-tcc).
#

set -u

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TESTS=$ROOT/tests

ALL_TESTS="ata cpu syscalls memory oom kvmem clock irq smp net tcp ext2 minix libc system disk tcc curl bash"

ARCH=x86_64
build=1
# what QEMU's machine is, besides its default
MACHINE=
# the freestanding tests' compiler (and what x86's needs), the tools needed
TEST_CC=gcc
TEST_CFLAGS=-fcf-protection=none
TOOLS="gcc ld cpio"
verbose=0
kargs=
smp=
jobs=1
# the kernel's compiler (x86): gcc, or tcc
KCC=gcc

# tests that measure time (a tick's rate, the clock against the host's,
# threads running at once) or use much memory: never beside others
SERIAL_TESTS="clock irq smp memory"

while getopts "a:C:c:j:k:nvh" opt; do
    case $opt in
        a) ARCH=$OPTARG ;;
        C) KCC=$OPTARG ;;
        c) smp=$OPTARG ;;
        j) jobs=$OPTARG ;;
        k) kargs=$OPTARG ;;
        n) build=0 ;;
        v) verbose=1 ;;
        *) sed -n '3,22p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
    esac
done
shift $((OPTIND - 1))

case $ARCH in
    i386)
        OUT=$TESTS/out
        QEMU=qemu-system-i386
        CPU_MODEL=qemu32
        HOST_CC_ARCH=-m32
        # the memory test: more RAM than the direct map's 768 MiB, which
        # are used, 512 MiB of them filled
        MEMORY_TEST_RAM=2G MEMORY_TEST_FILL=512
        CROSS_CC=$ROOT/build/toolchain/bin/i686-aquila-gcc ;;
    x86_64)
        OUT=$TESTS/out/x86_64
        QEMU=qemu-system-x86_64
        CPU_MODEL=qemu64
        HOST_CC_ARCH=-m64
        # more than 4 GiB filled: pages above 4 GiB are used
        MEMORY_TEST_RAM=6G MEMORY_TEST_FILL=4608
        CROSS_CC=$ROOT/build/toolchain/bin/x86_64-aquila-gcc ;;
    aarch64)
        OUT=$TESTS/out/aarch64
        QEMU=qemu-system-aarch64
        MACHINE="-M virt -cpu cortex-a72"
        TEST_CC="clang --target=aarch64-none-elf -fuse-ld=lld"
        TEST_CFLAGS=
        TOOLS="clang ld.lld llvm-objcopy cpio"
        ALL_TESTS="aarch64 oom"
        CROSS_CC=
        # no KVM: another CPU
        KVM=0 ;;
    *)
        echo "error: unknown architecture: $ARCH" >&2
        exit 2 ;;
esac

case $ARCH in
    aarch64) CONFIG=aarch64-virt ;;
    *)       CONFIG=../tests/host/$ARCH-pc ;;
esac

# the kernel built with tcc: its own configuration and build directory
case $KCC in
    gcc) ;;
    tcc)
        if [ "$ARCH" = aarch64 ]; then
            echo "error: -C tcc is x86's only" >&2
            exit 2
        fi
        CONFIG=$CONFIG-tcc
        OUT=$TESTS/out/$ARCH-tcc
        # the configuration's default: tcc, or its i386 compiler
        [ "$ARCH" = i386 ] && tcc=${TCC:-i386-tcc} || tcc=${TCC:-tcc}
        TOOLS="$TOOLS ${tcc%% *}" ;;
    *)
        echo "error: unknown compiler: $KCC" >&2
        exit 2 ;;
esac

# the main build (make, make CONFIG=x86_64-pc), contrib/ and ports/ come
# from there
case $ARCH in
    i386) MAIN_BUILD=$ROOT/build ;;
    *)    MAIN_BUILD=$ROOT/build/$ARCH ;;
esac

selected=${*:-$ALL_TESTS}

for tool in $TOOLS $QEMU; do
    if ! command -v $tool >/dev/null; then
        echo "error: $tool not found" >&2
        exit 2
    fi
done

mkdir -p "$OUT"

# hardware virtualization if there is any
ACCEL=
if [ "${KVM:-1}" != 0 ] && [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-accel kvm"
fi

#
# Kernel
#

if [ $build = 1 ]; then
    echo "building kernel (CONFIG=$CONFIG)"
    if ! make -C "$ROOT" kernel CONFIG=$CONFIG BUILDDIR="$OUT/build" > "$OUT/build.log" 2>&1; then
        grep -E -B2 -A6 'error|Error' "$OUT/build.log" | head -40
        echo "error: kernel build failed, see $OUT/build.log" >&2
        exit 1
    fi
fi

# the kernel's flat image if it has one (aarch64's), its ELF file else
KERNEL=$(ls "$OUT"/build/kernel/kernel-*.$ARCH.img 2>/dev/null | head -1)
[ -n "$KERNEL" ] || KERNEL=$(ls "$OUT"/build/kernel/kernel-*.$ARCH 2>/dev/null | head -1)

# -k, -c: QEMU through a wrapper that adds them to -append, and -smp (a
# script, timeout runs it); QEMU_BIN is QEMU itself
QEMU_BIN=$QEMU

if [ -n "$kargs" ] || [ -n "$smp" ]; then
    QEMU=$OUT/qemu-kargs
    cat > "$QEMU" <<EOF
#!/bin/bash
args=(${smp:+-smp $smp}) append=0 kernel=0
while [ \$# -gt 0 ]; do
    case "\$1" in
        -append) args+=(-append "\$2 $kargs"); append=1; shift 2; continue ;;
        -kernel) kernel=1 ;;
    esac
    args+=("\$1"); shift
done
[ \$append = 1 ] || [ \$kernel = 0 ] || [ -z "$kargs" ] || args+=(-append "$kargs")
exec $QEMU_BIN "\${args[@]}"
EOF
    chmod +x "$QEMU"
fi

if [ -z "$KERNEL" ]; then
    echo "error: no kernel image, run without -n" >&2
    exit 1
fi

#
# Helpers
#

# build tests/<name>/init.c into an initrd with /init
build_initrd()
{
    local name=$1
    local root=$OUT/$name/root

    rm -rf "$root"
    mkdir -p "$root/dev" "$root/mnt" "$root/tmp" "$root/proc"

    if [ "$name" = libc ]; then
        # a regular program, built against the C library
        "$CROSS_CC" -O1 -Wall -o "$root/init" "$TESTS/$name/init.c" \
            > "$OUT/$name/gcc.log" 2>&1 || { cat "$OUT/$name/gcc.log"; return 1; }
    else
        $TEST_CC ${HOST_CC_ARCH:-} -static -nostdlib -ffreestanding -fno-builtin -fno-pic -no-pie \
            -fno-stack-protector $TEST_CFLAGS -O1 -Wall -Wno-unused-function \
            -I"$ROOT/include/uapi" -Wl,--build-id=none -o "$root/init" "$TESTS/$name/init.c" \
            > "$OUT/$name/gcc.log" 2>&1 || { cat "$OUT/$name/gcc.log"; return 1; }
    fi

    # extra files for the test (e.g. etc/passwd)
    if [ -d "$TESTS/$name/rootfs" ]; then
        cp -r "$TESTS/$name/rootfs/." "$root/"
        [ -f "$root/etc/shadow" ] && chmod 600 "$root/etc/shadow"
    fi

    # how much the memory test fills, in MiB
    [ "$name" = memory ] && echo "$MEMORY_TEST_FILL" > "$root/size"

    # the kernel's initramfs reads old binary cpio archives
    (cd "$root" && find . | cpio -o -H bin -R 0:0 --quiet) > "$OUT/$name/initrd.img"
}

# run_qemu <log> <seconds> <stop pattern> [qemu args...]
#
# Boots the kernel, stops once <stop pattern> or a kernel panic shows up
# in the serial log, or after <seconds>. Sets qemu_exited=1 if QEMU went
# away on its own, which with -no-reboot means the guest reset (e.g. a
# triple fault).
run_qemu()
{
    local log=$1 secs=$2 pattern=$3
    shift 3

    rm -f "$log"
    $QEMU $ACCEL $MACHINE -kernel "$KERNEL" -m "${QEMU_MEM:-256}" -display none -no-reboot \
        -serial file:"$log" "$@" >/dev/null 2>&1 &

    local pid=$! ticks=0
    qemu_exited=0

    while kill -0 $pid 2>/dev/null; do
        if grep -a -q -E "$pattern|Bailing out" "$log" 2>/dev/null; then
            break
        fi

        if [ $ticks -ge $((secs * 10)) ]; then
            break
        fi

        sleep 0.1
        ticks=$((ticks + 1))
    done

    if kill -0 $pid 2>/dev/null; then
        kill $pid
    else
        qemu_exited=1
    fi

    wait $pid 2>/dev/null
}

# common failure checks on a serial log, prints the reason
log_problems()
{
    local log=$1

    if grep -a -q 'Bailing out' "$log"; then
        echo "kernel panic:"
        grep -a -B3 'Bailing out' "$log" | sed 's/^/    /'
    elif [ $qemu_exited = 1 ]; then
        echo "QEMU exited, guest reset (triple fault?)"
    else
        return 1
    fi
}

# check aqtest output: all checks passed and the test finished
aqtest_result()
{
    local log=$1

    log_problems "$log" && return 1

    local pass fail
    pass=$(grep -a -c '^PASS' "$log")
    fail=$(grep -a -c '^FAIL' "$log")

    grep -a '^FAIL' "$log" | sed 's/^/    /'

    if ! grep -a -q 'aqtest: done' "$log"; then
        echo "did not finish ($pass passed, $fail failed), last output:"
        tail -3 "$log" | sed 's/^/    /'
        return 1
    fi

    echo "$pass passed, $fail failed"
    [ "$fail" = 0 ]
}

#
# Tests
#

# a disk on the first ATA channel is detected reliably
test_ata()
{
    local log=$OUT/ata/serial.log
    local disk=$OUT/ata/disk.img
    local runs=5 ok=0

    mkdir -p "$OUT/ata"
    rm -f "$disk"
    truncate -s 16M "$disk"

    for i in $(seq $runs); do
        run_qemu "$log" 10 'registered blkdev 3: ata' -initrd "$OUT/cpu/initrd.img" \
            -drive file="$disk",format=raw,index=0,media=disk

        log_problems "$log" && return 1

        grep -a -q 'ata0: model: QEMU HARDDISK' "$log" && ok=$((ok + 1))
    done

    echo "disk detected in $ok/$runs boots"
    [ $ok = $runs ]
}

test_cpu()
{
    run_qemu "$OUT/cpu/serial.log" 30 'aqtest: done' -initrd "$OUT/cpu/initrd.img"
    aqtest_result "$OUT/cpu/serial.log"
}

# the generic kernel on aarch64: user mode, system calls, fork, exec,
# pipes, memory, faults, signals, the tick; an e1000 on PCI Express
test_aarch64()
{
    run_qemu "$OUT/aarch64/serial.log" 60 'aqtest: done' -initrd "$OUT/aarch64/initrd.img" \
        -append "ip=10.0.2.15/24:10.0.2.2" -netdev user,id=n0 -device e1000,netdev=n0
    aqtest_result "$OUT/aarch64/serial.log"
}

test_syscalls()
{
    run_qemu "$OUT/syscalls/serial.log" 60 'aqtest: done' -initrd "$OUT/syscalls/initrd.img"
    aqtest_result "$OUT/syscalls/serial.log"
}

# much memory used: pages above 4 GiB on x86-64
test_memory()
{
    QEMU_MEM=$MEMORY_TEST_RAM run_qemu "$OUT/memory/serial.log" 180 'aqtest: done' \
        -initrd "$OUT/memory/initrd.img"
    aqtest_result "$OUT/memory/serial.log"
}

# out of memory (128 MiB): a process that has none left is killed, the
# system goes on and has its memory back
test_oom()
{
    QEMU_MEM=128 run_qemu "$OUT/oom/serial.log" 120 'aqtest: done' -initrd "$OUT/oom/initrd.img"
    aqtest_result "$OUT/oom/serial.log"
}

# the kernel's allocator (mm/kvmem.c): its self-test at boot (kvmem.test),
# its objects back once processes, programs and threads are gone; a
# write past an object's end faults, at its guard page (kvmem.guard)
test_kvmem()
{
    local log=$OUT/kvmem/serial.log glog=$OUT/kvmem/guard.log

    run_qemu "$log" 60 'aqtest: done' -initrd "$OUT/kvmem/initrd.img" -append "kvmem.test"

    if ! grep -a -q 'kvmem: self-test passed' "$log"; then
        echo "self-test: $(grep -a 'kvmem: self-test' "$log" || echo 'not run')"
        return 1
    fi

    local result
    result=$(aqtest_result "$log") || { echo "$result"; return 1; }

    run_qemu "$glog" 30 'Bailing out' -initrd "$OUT/kvmem/initrd.img" -append "kvmem.guard"

    local at
    at=$(grep -a -o "writing past [^ ]* end, at [0-9a-fA-Fx]*" "$glog" | awk '{print $NF}')

    if [ -z "$at" ] || ! grep -a -q "CR2 = $at\|page fault at $at" "$glog"; then
        echo "a write past an object's end, at ${at:-?}: no fault there"
        tail -5 "$glog" | sed 's/^/    /'
        return 1
    fi

    echo "self-test passed, ${result%% failed*} failed; a write past an object's end faulted at its guard page"
}

# the clock, from each source: the TSC (invariant, QEMU's invtsc), the
# HPET (QEMU's default), the PIT's ticks (no HPET); tests/clock/init.c
# reads it, the host timestamps its lines (stamp.py) to compare
test_clock()
{
    local fails=0 summary=""

    # name, QEMU's options, the kernel's command line, the source, fine
    # (finer than a tick)
    local modes=(
        "tsc|-cpu $CPU_MODEL,+invtsc||tsc|1"
        "hpet|||hpet|1"
        "pit|-machine hpet=off||timer ticks|0"
        "forced|-cpu $CPU_MODEL,+invtsc|clock=hpet|hpet|1"
    )

    for m in "${modes[@]}"; do
        local name args karg want fine
        IFS='|' read -r name args karg want fine <<< "$m"
        local log=$OUT/clock/$name.log

        # QEMU (timeout's) is the job, its output timestamped beside it: a
        # pipeline's job would be waited for until QEMU's timeout, killing
        # the stamper doesn't stop QEMU
        timeout 60 $QEMU $ACCEL $args -kernel "$KERNEL" -initrd "$OUT/clock/initrd.img" -m 256 \
            -display none -no-reboot -append "$karg" -serial stdio 2>/dev/null \
            > >(python3 "$TESTS/clock/stamp.py" > "$log") &
        local pid=$!

        for _ in $(seq 300); do
            grep -a -q 'aqtest: done\|Bailing out' "$log" 2>/dev/null && break
            sleep 0.1
        done

        kill $pid 2>/dev/null
        wait $pid 2>/dev/null

        local problem
        problem=$(python3 - "$log" "$want" "$fine" <<'PY'
import re, sys
log, want, fine = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
lines = open(log, errors="replace").read().splitlines()
text = "\n".join(l.split(" ", 1)[1] if " " in l else l for l in lines)
def mark(name):
    for l in lines:
        m = re.match(r"([0-9.]+) %s ns=(\d+)" % name, l)
        if m:
            return float(m.group(1)), int(m.group(2)) / 1e9
out = []
if "aqtest: done" not in text:
    out.append("did not finish")
if re.search(r"^FAIL", text, re.M):
    out += re.findall(r"^FAIL.*", text, re.M)
if not re.search(r"^clock: %s" % want, text, re.M):
    out.append("not clock: %s (%s)" % (want, "; ".join(re.findall(r"^clock:.*", text, re.M))))
r = re.search(r"resolution_ns=(\d+)", text)
if r and fine and int(r.group(1)) >= 1000000:
    out.append("resolution %s ns, not finer than a tick" % r.group(1))
m0, m1 = mark("mark0"), mark("mark1")
if m0 and m1:
    host, guest = m1[0] - m0[0], m1[1] - m0[1]
    if abs(guest - host) > 0.05 * host:
        out.append("2 s: the guest counted %.3f s, the host %.3f s" % (guest, host))
elif "did not finish" not in out:
    out.append("no marks")
print("; ".join(out))
PY
)
        if [ -n "$problem" ]; then
            echo "$name: $problem"
            fails=$((fails + 1))
        else
            summary="$summary $name($(grep -a -o 'resolution_ns=[0-9]*' "$log" | cut -d= -f2) ns)"
        fi
    done

    [ $fails = 0 ] && echo "sources and their resolution:$summary; monotonic, nanosleep, 2 s as the host counts them"
    [ $fails = 0 ]
}

# interrupts through the APIC (the I/O APIC's routes, the local APIC's
# timer) and through the 8259 (apic=off): tests/irq/init.c shows
# /proc/interrupts, the timer's rate, the e1000's level-triggered line
test_irq()
{
    local fails=0 summary=""

    # name, QEMU's machine and NIC, the kernel's command line, the
    # controller, the timer's line, the NIC's (the 82540EM's line, IRQ 11;
    # the 82574L's MSI, IRQ 24 past the I/O APIC's 24 pins)
    local modes=(
        "apic|-device e1000,netdev=n0||apic|timer:|11: [1-9][0-9]* ioapic gsi 11 level"
        "8259|-device e1000,netdev=n0|apic=off|8259|0:|11: [1-9][0-9]* 8259 edge"
        "msi|-device e1000e,netdev=n0||apic|timer:|24: [1-9][0-9]* msi"
        "msi-8259|-device e1000e,netdev=n0|apic=off|8259|0:|11: [1-9][0-9]* 8259 edge"
        "q35|-machine q35 -device e1000e,netdev=n0||apic|timer:|24: [1-9][0-9]* msi"
    )

    for m in "${modes[@]}"; do
        local name qargs karg want timer e1000
        IFS='|' read -r name qargs karg want timer e1000 <<< "$m"
        local log=$OUT/irq/$name.log

        rm -f "$log"
        $QEMU_BIN $ACCEL $qargs -kernel "$KERNEL" -initrd "$OUT/irq/initrd.img" -m 256 -display none \
            -no-reboot -append "ip=10.0.2.15/24:10.0.2.2 $karg" -serial file:"$log" \
            -netdev user,id=n0 >/dev/null 2>&1 &
        local pid=$!

        for _ in $(seq 300); do
            grep -a -q 'aqtest: done\|Bailing out' "$log" 2>/dev/null && break
            sleep 0.1
        done

        kill $pid 2>/dev/null
        wait $pid 2>/dev/null

        local problem=""
        grep -a -q 'aqtest: done, fails=0' "$log" || problem="$problem; did not pass ($(grep -a '^FAIL\|Bailing' "$log" | head -2 | tr '\n' ' '))"
        grep -a -q "^controller: $want" "$log" || problem="$problem; not through the $want"
        grep -a -q -E "^$e1000" "$log" || problem="$problem; the NIC's IRQ: $(grep -a '^\(11\|24\):' "$log" | tr '\n' ' ')"

        local rate
        rate=$(grep -a -o 'timer_rate=[0-9-]*' "$log" | cut -d= -f2)
        [ -n "$rate" ] && [ "$rate" -ge 450 ] && [ "$rate" -le 550 ] || problem="$problem; the timer ticked $rate times a second"

        if [ -n "$problem" ]; then
            echo "$name:${problem#;}"
            fails=$((fails + 1))
        else
            summary="$summary $name($rate/s)"
        fi
    done

    [ $fails = 0 ] && echo "through the apic and the 8259:$summary; the 82540EM's IRQ 11 (level under the apic), the 82574L's MSI (its line with the 8259), q35"
    [ $fails = 0 ]
}

# the other CPUs started (cpu/smp.c), 4 of them, with the pc and q35
# machines, running tests/smp/init.c (threads at once, the TLBs flushed
# on each, processes killed while they run, signals, the FPU); smp=off,
# and apic=off (no local APIC to start them with): the boot CPU only, the
# cpu test's programs pass.
test_smp()
{
    local fails=0

    # name, QEMU's arguments, the kernel's command line, the smp line
    # (none: no line), the test
    local modes=(
        "pc|-smp 4||smp: 4 CPUs|smp"
        "q35|-machine q35 -smp 4||smp: 4 CPUs|smp"
        "smp=off|-smp 4|smp=off|smp: off (smp=off), 4 CPUs|cpu"
        "apic=off|-smp 4|apic=off||cpu"
    )

    for m in "${modes[@]}"; do
        local name qargs karg want test
        IFS='|' read -r name qargs karg want test <<< "$m"
        local log=$OUT/smp/${name%%=*}.log

        rm -f "$log"
        $QEMU_BIN $ACCEL $qargs -kernel "$KERNEL" -initrd "$OUT/$test/initrd.img" -m 256 \
            -display none -no-reboot -append "$karg" -serial file:"$log" >/dev/null 2>&1 &
        local pid=$!

        for _ in $(seq 300); do
            grep -a -q 'aqtest: done\|Bailing out' "$log" 2>/dev/null && break
            sleep 0.1
        done

        kill $pid 2>/dev/null
        wait $pid 2>/dev/null

        local problem="" got
        got=$(grep -a '^smp: ' "$log" | head -1 | tr -d '\r')
        grep -a -q 'aqtest: done, fails=0' "$log" ||
            problem="$problem; did not pass ($(grep -a '^FAIL\|Bailing' "$log" | head -2 | tr '\n' ' '))"
        [ "$got" = "${want}" ] || problem="$problem; '${got:-no smp line}', not '${want:-no smp line}'"

        if [ -n "$problem" ]; then
            echo "$name: ${problem#; }"
            fails=$((fails + 1))
        fi
    done

    echo "4 CPUs (pc, q35): $(grep -a -c '^PASS' "$OUT/smp/pc.log") checks passed, threads at once $(grep -a '^at once:' "$OUT/smp/pc.log" | tr -dc '0-9')%; the boot CPU only with smp=off and apic=off"
    [ $fails = 0 ]
}

# the e1000 and the stack over it: QEMU gives its frames to a peer on the
# host (tests/net/peer.py, -netdev socket), which checks the answers to
# ARP and pings, then answers the guest's program (tests/net/init.c); both
# report their checks. Twice: the 82540EM (e1000, its line) and the
# 82574L (e1000e, MSI)
test_net()
{
    local out e1000 e1000e

    e1000=$(net_with e1000) || { echo "e1000:"; echo "$e1000" | sed 's/^/    /'; return 1; }
    e1000e=$(net_with e1000e) || { echo "e1000e:"; echo "$e1000e" | sed 's/^/    /'; return 1; }

    echo "e1000: $e1000; e1000e (msi with the apic): $e1000e"
}

net_with()
{
    local nic=$1
    local log=$OUT/net/serial-$nic.log peerlog=$OUT/net/peer-$nic.log
    local port=$((20000 + RANDOM % 20000))

    mkdir -p "$OUT/net"
    rm -f "$log"

    $QEMU $ACCEL -kernel "$KERNEL" -initrd "$OUT/net/initrd.img" -m 256 -display none \
        -no-reboot -append "ip=10.0.2.15/24:10.0.2.2" -serial file:"$log" \
        -netdev socket,id=n0,udp=127.0.0.1:$port,localaddr=127.0.0.1:$((port + 1)) \
        -device $nic,netdev=n0 >/dev/null 2>&1 &
    local pid=$!

    for _ in $(seq 300); do
        grep -a -q 'net: eth0 10.0.2.15\|Bailing out' "$log" 2>/dev/null && break
        sleep 0.1
    done

    python3 "$TESTS/net/peer.py" $port $((port + 1)) > "$peerlog" 2>&1

    for _ in $(seq 100); do
        grep -a -q 'aqtest: done\|Bailing out' "$log" 2>/dev/null && break
        sleep 0.1
    done

    kill $pid 2>/dev/null
    wait $pid 2>/dev/null
    qemu_exited=0

    log_problems "$log" && return 1

    local pass fail
    pass=$(cat "$peerlog" "$log" | grep -a -c '^PASS')
    fail=$(cat "$peerlog" "$log" | grep -a -c '^FAIL')
    cat "$peerlog" "$log" | grep -a '^FAIL' | sed 's/^/    /'

    if ! grep -q 'net: done' "$peerlog"; then
        echo "the peer did not finish ($pass passed, $fail failed):"
        tail -3 "$peerlog" | sed 's/^/    /'
        return 1
    fi

    if ! grep -a -q 'aqtest: done' "$log"; then
        echo "the guest did not finish ($pass passed, $fail failed):"
        tail -3 "$log" | sed 's/^/    /'
        return 1
    fi

    echo "$pass passed, $fail failed"
    [ "$fail" = 0 ]
}

# TCP against another one: QEMU's user network has its own; the guest
# (tests/tcp/init.c) and tests/tcp/host.py exchange megabytes through it
# (guestfwd and hostfwd), both report their checks
test_tcp()
{
    local log=$OUT/tcp/serial.log
    local hport=$((20000 + RANDOM % 20000)) gport=$((40000 + RANDOM % 20000))

    mkdir -p "$OUT/tcp"
    rm -f "$log"

    python3 "$TESTS/tcp/host.py" $hport $gport > "$OUT/tcp/host.log" 2>&1 &
    local host=$!

    # QEMU connects to its listener when it starts (guestfwd)
    for _ in $(seq 100); do
        grep -q listening "$OUT/tcp/host.log" 2>/dev/null && break
        sleep 0.1
    done

    $QEMU $ACCEL -kernel "$KERNEL" -initrd "$OUT/tcp/initrd.img" -m 256 -display none \
        -no-reboot -append "ip=10.0.2.15/24:10.0.2.2" -serial file:"$log" \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$gport-:7000,guestfwd=tcp:10.0.2.100:7001-tcp:127.0.0.1:$hport \
        -device e1000,netdev=n0 >/dev/null 2>&1 &
    local pid=$!

    for _ in $(seq 1800); do
        grep -a -q 'aqtest: done\|Bailing out' "$log" 2>/dev/null && ! kill -0 $host 2>/dev/null && break
        sleep 0.1
    done

    kill $pid $host 2>/dev/null
    wait $pid $host 2>/dev/null
    qemu_exited=0

    log_problems "$log" && return 1

    local pass fail
    pass=$(cat "$OUT/tcp/host.log" "$log" | grep -a -c '^PASS')
    fail=$(cat "$OUT/tcp/host.log" "$log" | grep -a -c '^FAIL')
    cat "$OUT/tcp/host.log" "$log" | grep -a '^FAIL' | sed 's/^/    /'

    if ! grep -q 'tcp: done' "$OUT/tcp/host.log"; then
        echo "the host did not finish ($pass passed, $fail failed):"
        tail -3 "$OUT/tcp/host.log" | sed 's/^/    /'
        return 1
    fi

    if ! grep -a -q 'aqtest: done' "$log"; then
        echo "the guest did not finish ($pass passed, $fail failed):"
        tail -3 "$log" | sed 's/^/    /'
        return 1
    fi

    echo "$pass passed, $fail failed"
    [ "$fail" = 0 ]
}

test_ext2()
{
    local img=$OUT/ext2/disk.img
    local log=$OUT/ext2/serial.log

    for tool in mke2fs e2fsck debugfs python3; do
        if ! command -v $tool >/dev/null; then
            echo "SKIP: $tool not found"
            return 0
        fi
    done

    rm -f "$img"
    mke2fs -q -t ext2 -b 1024 -F "$img" 4M || return 1

    run_qemu "$log" 60 'aqtest: done' -initrd "$OUT/ext2/initrd.img" \
        -drive file="$img",format=raw,index=0,media=disk

    aqtest_result "$log" || return 1

    # check the image on the host
    if ! e2fsck -fn "$img" > "$OUT/ext2/e2fsck.log" 2>&1; then
        echo "e2fsck found errors:"
        sed 's/^/    /' "$OUT/ext2/e2fsck.log"
        return 1
    fi

    debugfs -R "dump /big $OUT/ext2/big.out" "$img" >/dev/null 2>&1

    python3 -I - "$OUT/ext2/big.out" <<'EOF' || return 1
import sys
data = open(sys.argv[1], 'rb').read()
want = bytes((i * 7 + (i >> 9)) & 0xff for i in range(305000))
if data != want:
    bad = next((i for i in range(min(len(data), len(want))) if data[i] != want[i]), None)
    print("file contents differ on the host: size %d, first mismatch at %s" % (len(data), bad))
    sys.exit(1)
EOF

    # what the test left: removed, renamed and unlinked-while-open files gone
    local names
    names=$(debugfs -R "ls /" "$img" 2>/dev/null | tr -s ' ' '\n' | grep -v '^[0-9(]' | sort | xargs)
    if [ "$names" != ". .. a b big devnode huge lost+found sym trunc" ]; then
        echo "unexpected files on the image: $names"
        return 1
    fi

    # the device node's number, where Linux reads it
    if ! debugfs -R "stat /devnode" "$img" 2>/dev/null | grep -q 'major/minor number: 03:01'; then
        echo "the device node's number isn't Linux's 3, 1"
        return 1
    fi

    echo "e2fsck clean, file contents and directory verified on the host"
}

# minix (v1, 1K zones) on the first disk: tests/minix/init.c writes,
# reads, makes holes and truncates; fsck.minix checks the image on the
# host (its zone and inode maps against the files)
test_minix()
{
    local img=$OUT/minix/disk.img
    local log=$OUT/minix/serial.log

    if ! command -v mkfs.minix >/dev/null || ! command -v fsck.minix >/dev/null; then
        echo "SKIP: mkfs.minix or fsck.minix not found"
        return 0
    fi

    rm -f "$img"
    truncate -s 4M "$img"
    mkfs.minix -1 -i 128 "$img" >/dev/null || return 1

    run_qemu "$log" 60 'aqtest: done' -initrd "$OUT/minix/initrd.img" \
        -drive file="$img",format=raw,index=0,media=disk

    aqtest_result "$log" || return 1

    # the guest used every inode: each one's bit is set, the last one's
    # too (bit i is inode i, from 1 to s_ninodes; the bitmap is block 2)
    if ! python3 - "$img" <<'PY'
import struct, sys
img = open(sys.argv[1], "rb").read()
ninodes = struct.unpack_from("<H", img, 1024)[0]
imap = img[2048:]
free = [i for i in range(1, ninodes + 1) if not imap[i // 8] & (1 << (i % 8))]
if free:
    print("inodes left free:", free[:10])
    sys.exit(1)
PY
    then
        return 1
    fi

    if ! fsck.minix -f "$img" > "$OUT/minix/fsck.log" 2>&1; then
        echo "fsck.minix found errors:"
        sed 's/^/    /' "$OUT/minix/fsck.log"
        return 1
    fi

    echo "$(grep -a -c '^PASS' "$log") passed, fsck.minix clean"
}

# C library: errno, signals, stat and library functions through newlib
test_libc()
{
    if [ ! -x "$CROSS_CC" ]; then
        echo "SKIP: no ${CROSS_CC##*/} toolchain, run make toolchain"
        return 0
    fi

    run_qemu "$OUT/libc/serial.log" 30 'aqtest: done' -initrd "$OUT/libc/initrd.img"
    aqtest_result "$OUT/libc/serial.log"
}

# the system built from source: logins on the serial console, run commands
#
# tests/system/rootfs adds a root password and an unprivileged guest user,
# dash and sbase (contrib/) come from the main build: make
test_system()
{
    local log=$OUT/system/serial.log
    local initrd=$OUT/system/initrd.img

    if [ ! -x "$CROSS_CC" ]; then
        echo "SKIP: no ${CROSS_CC##*/} toolchain, run make toolchain"
        return 0
    fi

    # the base system's third-party software from the main build (make),
    # without the optional ports
    local contrib=$MAIN_BUILD/contrib/root
    if [ ! -x "$contrib/bin/dash" ]; then
        echo "SKIP: contrib not built, run make"
        return 0
    fi

    mkdir -p "$OUT/system"
    make -C "$ROOT" initrd CONFIG=$ARCH-pc BUILDDIR="$OUT/system/build" TOOLCHAIN="$ROOT/build/toolchain" \
        CONTRIB_ROOT="$contrib" PORTS_ROOT=/nonexistent ROOTFS_OVERLAY="$TESTS/system/rootfs" \
        > "$OUT/system/build.log" 2>&1 || {
        tail -20 "$OUT/system/build.log"
        return 1
    }
    cp "$OUT/system/build/initrd.img" "$initrd"

    # output is checked for what the shell expanded, not the echoed input
    local long=0123456789012345678901234567890123456789012345678901234567890123456789

    # "seconds input": wait, then type the line on the serial console
    local session=(
        "6 root" "1 wrong-password"
        "6 guest" "1 guest-password" "5 echo user=\$USER" "1 cat /etc/shadow"
        "1 touch /tmp/g; ls -l /tmp/g" "1 echo via-\$((1 + 1))-tty > /dev/tty" "1 exit"
        "4 root" "1 root-password" "5 echo user=\$USER" "1 uname -a" "1 ls /bin" "1 ps"
        "1 cat /proc/meminfo" "1 touch /tmp/f" "1 ls /tmp" "1 cat /etc/shadow"
        "1 echo pipe-\$((6 * 7)) | cat | cat" "1 echo subst=\$(echo inner | cat)"
        "1 echo long:\$USER:$long:end"
        "1 cat" "1 "$'\x03' "1 echo status=\$?"
        "1 cd /tmp; mkdir d; echo sbase > d/f; cp -r d e; mv e/f e/g; ls e"
        "1 ln -s d/f l; cat l; rm -r d e; ls -l l"
        "1 seq 3 | sort -r | paste -sd, -" "1 yes | head -n 2 | wc -l"
        "1 sleep 30" "1 "$'\x1a' "1 jobs" "1 kill -9 %1"
        "1 flock /tmp/lk sh -c 'flock -n /tmp/lk true || echo lock-busy'"
        "1 sv status /etc/service/getty-ttyS0"
        "1 sv status /etc/service/dhcp; ifconfig eth0; ping -c 1 10.0.2.2; cat /etc/resolv.conf"
        "1 nc -l 127.0.0.1 9000 > /tmp/nc &" "1 echo via-nc-\$((40 + 2)) | nc -N 127.0.0.1 9000; cat /tmp/nc"
        "3 poweroff"
    )

    {
        for step in "${session[@]}"; do
            sleep "${step%% *}"
            printf '%s\r' "${step#* }"
        done
        sleep 2
    } | timeout 90 $QEMU $ACCEL -kernel "$KERNEL" -initrd "$initrd" \
        -append earlycon=ttyS0 -m 256 -display none -no-reboot -serial stdio \
        2>/dev/null | tr -d '\r' > "$log"

    if grep -a -q 'Bailing out' "$log"; then
        echo "kernel panic:"
        grep -a -B3 'Bailing out' "$log" | sed 's/^/    /'
        return 1
    fi

    local missing=0
    for want in 'Login incorrect' 'user=guest' 'shadow: Permission denied' \
                'user=root' 'AquilaOS aquila' 'aqbox' '/bin/ps' 'MemTotal' \
                '(^| )f( |$)' 'guest:\$6\$' 'pipe-42' 'subst=inner' \
                "long:root:$long:end" 'status=130' 'guest +guest .* /tmp/g' \
                '^g$' '(^|# )sbase$' 'l -> d/f' '^3,2,1$' '^2$' '^via-2-tty$' \
                'Stopped +sleep 30' '^lock-busy$' 'run: /etc/service/getty-ttyS0: \(pid [0-9]+\)' \
                'run: /etc/service/dhcp: \(pid [0-9]+\)' 'inet 10\.0\.2\.15 +netmask 255\.255\.255\.0' \
                '1 packets transmitted, 1 received' '^nameserver 10\.0\.2\.3$' '^via-nc-42$' \
                'kernel: power down'; do
        if ! grep -a -q -E -- "$want" "$log"; then
            echo "missing output: $want"
            missing=1
        fi
    done

    [ $missing = 0 ] && echo "wrong password rejected, guest denied /etc/shadow, root ran uname, ls, ps, cat, touch, pipelines, \$(...), a long line, ^C, ^Z, /dev/tty, sbase, flock, sv, DHCP (sdhcp), nc and poweroff"
}

# the system installed on a hard disk (make hdd): booted from it with
# Limine, a file written on the first boot is there on the second (after
# make hdd-kernel's and hdd-update's update of the kernel and the system,
# which adds a file), and the root filesystem is clean afterwards
test_disk()
{
    local img=$OUT/disk/aquila.img

    if [ ! -x "$CROSS_CC" ] || [ ! -x "$ROOT/build/toolchain/bin/limine" ]; then
        echo "SKIP: no ${CROSS_CC##*/} toolchain, run make toolchain"
        return 0
    fi

    local contrib=$MAIN_BUILD/contrib/root
    if [ ! -x "$contrib/bin/dash" ]; then
        echo "SKIP: contrib not built, run make"
        return 0
    fi

    for tool in sfdisk mkfs.fat mcopy mke2fs fakeroot e2fsck debugfs python3; do
        if ! command -v $tool >/dev/null; then
            echo "SKIP: $tool not found"
            return 0
        fi
    done

    mkdir -p "$OUT/disk"
    make -C "$ROOT" initrd CONFIG=$ARCH-pc BUILDDIR="$OUT/disk/build" TOOLCHAIN="$ROOT/build/toolchain" \
        CONTRIB_ROOT="$contrib" PORTS_ROOT=/nonexistent > "$OUT/disk/build.log" 2>&1 &&
    "$ROOT/mk/hdd.sh" "$img" 128 "$OUT/disk/build/rootfs" "$KERNEL" \
        "$ROOT/boot/limine-hdd.conf" "$ROOT/build/toolchain/share/limine" \
        "$ROOT/build/toolchain/bin/limine" >> "$OUT/disk/build.log" 2>&1 || {
        tail -20 "$OUT/disk/build.log"
        return 1
    }

    # wait_for <log> <count> <pattern>: until the pattern is on <count>
    # lines of the log, at most a minute
    wait_for()
    {
        for _ in $(seq 600); do
            [ "$(grep -a -c -E -- "$3" "$1")" -ge "$2" ] && return 0
            sleep 0.1
        done
        return 1
    }

    # boot <log> <command>: log in as root on the serial port once there
    # is a prompt, run the command and power off
    boot()
    {
        local fifo=$OUT/disk/input

        rm -f "$fifo"
        mkfifo "$fifo"
        # straight to the log, a pipe would hold back what the waits look for
        timeout 120 $QEMU $ACCEL -hda "$img" -m 256 -display none -no-reboot \
            -serial stdio < "$fifo" > "$1.raw" 2>/dev/null &
        local pid=$!

        exec 3> "$fifo"
        wait_for "$1.raw" 1 'login: ' && printf 'root\r' >&3 &&
        wait_for "$1.raw" 1 '^# ' && printf '%s\r' "$2" >&3 &&
        wait_for "$1.raw" 2 '^# ' && printf 'poweroff\r' >&3
        wait $pid
        exec 3>&-

        tr -d '\r' < "$1.raw" > "$1"
    }

    # no sync: powering off writes what the block cache has
    boot "$OUT/disk/serial.log" 'echo disk-$((6 * 7)) > /root/kept'

    # a new kernel and system (make hdd-update): a file the system has now
    # is on the disk, what was made on it is kept
    rm -rf "$OUT/disk/rootfs2"
    cp -a "$OUT/disk/build/rootfs" "$OUT/disk/rootfs2"
    echo updated > "$OUT/disk/rootfs2/etc/updated"
    { "$ROOT/mk/hdd-kernel.sh" "$img" "$KERNEL" "$ROOT/boot/limine-hdd.conf" &&
      "$ROOT/mk/hdd-update.sh" "$img" "$OUT/disk/rootfs2"; } >> "$OUT/disk/build.log" 2>&1 || {
        tail -5 "$OUT/disk/build.log"
        return 1
    }

    boot "$OUT/disk/serial2.log" 'cat /root/kept /etc/updated'

    local root_start
    root_start=$(sfdisk -d "$img" | sed -n 's/.*img2 : start= *\([0-9]*\).*/\1/p')
    dd if="$img" of="$OUT/disk/root.img" bs=512 skip="$root_start" status=none

    if ! grep -a -q '^updated$' "$OUT/disk/serial2.log"; then
        echo "the file the update added is missing"
        tail -5 "$OUT/disk/serial2.log" | sed 's/^/    /'
        return 1
    fi

    if ! grep -a -q '^disk-42$' "$OUT/disk/serial2.log"; then
        echo "the file written on the first boot is missing"
        tail -5 "$OUT/disk/serial2.log" | sed 's/^/    /'
        return 1
    fi

    if ! e2fsck -fn "$OUT/disk/root.img" > "$OUT/disk/e2fsck.log" 2>&1; then
        echo "e2fsck found errors:"
        sed 's/^/    /' "$OUT/disk/e2fsck.log" | head -20
        return 1
    fi

    echo "booted from the disk twice, the file written on the first boot kept (a kernel and system update between), e2fsck clean"
}

# ports_initrd <test> [overlay]: an initrd of the system with the ports
# (make ports), and the overlay's files, in $OUT/<test>/build
ports_initrd()
{
    local dir=$OUT/$1

    mkdir -p "$dir"
    make -C "$ROOT" initrd CONFIG=$ARCH-pc BUILDDIR="$dir/build" TOOLCHAIN="$ROOT/build/toolchain" \
        CONTRIB_ROOT="$MAIN_BUILD/contrib/root" PORTS_ROOT="$MAIN_BUILD/ports/root" \
        ${2:+ROOTFS_OVERLAY="$2"} > "$dir/build.log" 2>&1 || {
        tail -20 "$dir/build.log"
        return 1
    }
}

# ports_session <test> [qemu args...]: boots that initrd, logs in as root
# on the serial console and types the commands read from stdin, each once
# the shell prompts again, then powers off; the session is in
# $OUT/<test>/serial.log
ports_session()
{
    local dir=$OUT/$1
    shift
    local log=$dir/serial.log fifo=$dir/input

    rm -f "$fifo" "$log.raw"
    mkfifo "$fifo"
    timeout 120 $QEMU $ACCEL -kernel "$KERNEL" -initrd "$dir/build/initrd.img" \
        -append earlycon=ttyS0 -m 256 -display none -no-reboot -serial stdio "$@" \
        < "$fifo" > "$log.raw" 2>/dev/null &
    local pid=$!

    prompts()
    {
        for _ in $(seq 600); do
            [ "$(grep -a -c '^# ' "$log.raw")" -ge "$1" ] && return 0
            sleep 0.1
        done
        return 1
    }

    exec 3> "$fifo"
    local n=1 line
    for _ in $(seq 600); do grep -a -q 'login: ' "$log.raw" && break; sleep 0.1; done
    printf 'root\r' >&3
    prompts 1
    while IFS= read -r line; do
        printf '%s\r' "$line" >&3
        n=$((n + 1))
        prompts $n || break
    done
    printf 'poweroff\r' >&3
    wait $pid
    exec 3>&-
    tr -d '\r' < "$log.raw" > "$log"
}

# expect_lines <log> <pattern...>: every pattern (an extended regular
# expression) is in the log, else which aren't
expect_lines()
{
    local log=$1 missing=0 want
    shift

    for want in "$@"; do
        if ! grep -a -q -E -- "$want" "$log"; then
            echo "missing output: $want"
            missing=1
        fi
    done

    return $missing
}

# the port's program, else why the test is skipped (printed, 0)
port_present()
{
    if [ ! -x "$CROSS_CC" ]; then
        echo "SKIP: no ${CROSS_CC##*/} toolchain, run make toolchain"
        return 1
    fi

    if [ ! -x "$MAIN_BUILD/ports/root$2" ]; then
        echo "SKIP: the $1 port isn't built, run make ports"
        return 1
    fi
}

# the tcc port (make ports) on AquilaOS: compiling and running programs,
# with the C library and libm
test_tcc()
{
    port_present tcc /usr/bin/tcc || return 0
    ports_initrd tcc || return 1

    ports_session tcc <<'END'
cd /tmp; printf '#include <stdio.h>\nint main(void) { printf("tcc-%%d\\n", 6 * 7); return 0; }\n' > a.c
tcc -o a a.c && ./a
printf '#include <stdio.h>\n#include <math.h>\nint main(void) { long long b = 1LL << 40; printf("sqrt=%%.1f div=%%lld\\n", sqrt(2.25), b / 3); return 0; }\n' > m.c
tcc -o m m.c -lm && ./m
END

    expect_lines "$OUT/tcc/serial.log" '^tcc-42$' '^sqrt=1\.5 div=366503875925$' &&
        echo "compiled and ran programs on AquilaOS, with libc and libm"
}

# HTTPS with the curl port (TLS through Mbed TLS), from the system to
# servers on the host (10.0.2.2): a certificate for aquila.test from a CA
# made for the test, which curl is given, and one for wrong.test; the
# page fetched over TLS 1.3, the wrong name and the unknown CA refused
test_curl()
{
    local dir=$OUT/curl

    port_present curl /usr/bin/curl || return 0

    rm -rf "$dir/pki" "$dir/overlay"
    mkdir -p "$dir/pki" "$dir/overlay/etc"

    # the CA, the server certificates it signs
    (
        cd "$dir/pki"
        openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj /CN=aquila-test-ca \
            -keyout ca.key -out ca.pem
        for name in aquila.test wrong.test; do
            openssl req -newkey rsa:2048 -nodes -subj "/CN=$name" -keyout "$name.key" -out "$name.csr"
            printf 'subjectAltName=DNS:%s\n' "$name" > "$name.ext"
            openssl x509 -req -in "$name.csr" -CA ca.pem -CAkey ca.key -CAcreateserial -days 2 \
                -extfile "$name.ext" -out "$name.pem"
        done
    ) > "$dir/pki.log" 2>&1 || { tail -5 "$dir/pki.log"; return 1; }

    cp "$dir/pki/ca.pem" "$dir/overlay/etc/test-ca.pem"
    ports_initrd curl "$dir/overlay" || return 1

    # two free ports, the servers on them
    local p1 p2
    read -r p1 p2 < <(python3 -c 'import socket
s = [socket.socket() for _ in range(2)]
[x.bind(("127.0.0.1", 0)) for x in s]
print(*[x.getsockname()[1] for x in s])')

    rm -f "$dir/ready1" "$dir/ready2"
    python3 "$TESTS/curl/server.py" "$dir/pki/aquila.test.pem" "$dir/pki/aquila.test.key" "$p1" "$dir/ready1" &
    local s1=$!
    python3 "$TESTS/curl/server.py" "$dir/pki/wrong.test.pem" "$dir/pki/wrong.test.key" "$p2" "$dir/ready2" &
    local s2=$!

    for _ in $(seq 100); do [ -f "$dir/ready1" ] && [ -f "$dir/ready2" ] && break; sleep 0.1; done

    ports_session curl -netdev user,id=n0 -device e1000,netdev=n0 <<END
ifconfig eth0 up; sdhcp eth0; sleep 3
curl -sS --cacert /etc/test-ca.pem --resolve aquila.test:$p1:10.0.2.2 https://aquila.test:$p1/
curl -sSv --cacert /etc/test-ca.pem --resolve aquila.test:$p1:10.0.2.2 -o /dev/null https://aquila.test:$p1/ 2>&1 | grep 'Handshake complete'
curl -sS --cacert /etc/test-ca.pem --resolve aquila.test:$p2:10.0.2.2 https://aquila.test:$p2/; echo "wrong-name=\$?"
curl -sS --resolve aquila.test:$p1:10.0.2.2 https://aquila.test:$p1/; echo "unknown-ca=\$?"
END

    kill $s1 $s2 2>/dev/null
    wait $s1 $s2 2>/dev/null

    expect_lines "$dir/serial.log" '^aquila-tls-ok$' 'mbedTLS: TLSv1\.3 Handshake complete' \
        '^wrong-name=60$' '^unknown-ca=60$' &&
        echo "HTTPS over TLS 1.3: the page fetched, the wrong name and the unknown CA refused"
}

# the bash port: a script of its features (tests/bash/test.sh) run as a
# program, its version and the machine it says it's on
test_bash()
{
    local dir=$OUT/bash

    port_present bash /bin/bash || return 0

    rm -rf "$dir/overlay"
    mkdir -p "$dir/overlay/usr/share"
    install -m 755 "$TESTS/bash/test.sh" "$dir/overlay/usr/share/bash-test.sh"
    ports_initrd bash "$dir/overlay" || return 1

    ports_session bash <<'END'
bash --version | head -1; bash -c 'echo "hosttype: $HOSTTYPE"'
/usr/share/bash-test.sh
END

    local cpu
    [ "$ARCH" = i386 ] && cpu=i686 || cpu=$ARCH

    expect_lines "$dir/serial.log" '^GNU bash, version 5\.3\.20' "^hosttype: $cpu\$" \
        '^arrays: one two three four n=8$' '^strings: hello, aquilaos AquilaOS Hello, bashOS 15$' \
        '^mapfile: 3 b read: x\|y z$' '^functions: 3628800$' '^procsub: left right $' \
        '^pipefail: 1$' '^jobs: 0 7$' '^trap: USR1$' '^globs: a.c b.h 1 2 3 007$' &&
        echo "bash 5.3: arrays, strings, functions, process substitution, jobs, traps and globs"
}

#
# Main
#

failed=0

for name in $selected; do
    case " $ALL_TESTS " in
        *" $name "*) ;;
        *) echo "unknown test: $name" >&2; exit 2 ;;
    esac
done

# test programs (the ata and smp tests boot the cpu test's initrd too)
needed=" $selected "
case "$needed" in *" ata "*|*" smp "*) needed="$needed cpu " ;; esac
[ -x "$CROSS_CC" ] || needed=${needed/ libc / }

for name in cpu syscalls memory oom kvmem clock irq smp net tcp ext2 minix libc aarch64; do
    case "$needed" in
        *" $name "*)
            mkdir -p "$OUT/$name"
            build_initrd $name || { echo "error: failed to build $name test" >&2; exit 1; } ;;
    esac
done

# each test's output and exit status, in $RESULTS
RESULTS=$OUT/results
rm -rf "$RESULTS"
mkdir -p "$RESULTS"

# run_test <name>: in a subshell of its own, which takes its background
# helpers (servers, peers, QEMU) with it when it ends, however it ends
run_test()
{
    local start=$SECONDS

    (
        trap 'kill $(jobs -p) 2>/dev/null' EXIT
        test_$1
    ) > "$RESULTS/$1.out" 2>&1
    local status=$?

    echo $((SECONDS - start)) > "$RESULTS/$1.time"
    echo $status > "$RESULTS/$1.status"
}

# report <name>: a finished test's line (and what it said, if it failed)
report()
{
    local name=$1 status
    status=$(cat "$RESULTS/$name.status")

    printf '%-10s %4ss ' "$name" "$(cat "$RESULTS/$name.time")"

    if [ "$status" = 0 ]; then
        echo "ok    $(tail -1 "$RESULTS/$name.out")"
    else
        echo "FAIL"
        sed 's/^/    /' "$RESULTS/$name.out"
        if [ $verbose = 1 ]; then
            echo "    --- serial log:"
            sed 's/^/    | /' "$OUT/$name/serial.log" 2>/dev/null | tail -40
        fi
        failed=$((failed + 1))
    fi
}

# the tests left to report, in order: each as soon as it and those
# before it are done
order=()
next=0

report_done()
{
    while [ $next -lt ${#order[@]} ] && [ -f "$RESULTS/${order[$next]}.status" ]; do
        report "${order[$next]}"
        next=$((next + 1))
    done
}

# interrupted: the tests running go too (their EXIT traps take their
# helpers)
trap 'kill $(jobs -p) 2>/dev/null; exit 130' INT TERM

if [ "$jobs" -le 1 ]; then
    for name in $selected; do
        order+=("$name")
        run_test "$name"
        report_done
    done
else
    # the others N at a time, then the serial ones, one by one
    parallel= serial=
    for name in $selected; do
        case " $SERIAL_TESTS " in
            *" $name "*) serial="$serial $name" ;;
            *)           parallel="$parallel $name" ;;
        esac
    done

    order=($parallel $serial)
    running=0

    for name in $parallel; do
        if [ $running -ge "$jobs" ]; then
            wait -n
            running=$((running - 1))
            report_done
        fi

        run_test "$name" &
        running=$((running + 1))
    done

    while [ $running -gt 0 ]; do
        wait -n
        running=$((running - 1))
        report_done
    done

    for name in $serial; do
        run_test "$name"
        report_done
    done
fi

[ $failed = 0 ] && echo "all tests passed" || echo "$failed test(s) failed"
exit $failed
