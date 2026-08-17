#!/usr/bin/env bash
# eval.sh - Evaluation harness for Adaptive Prefetch Guard
#
# Demonstrates the page-cache thrashing regression caused by oversized block
# read-ahead, and shows where apg's remediation helps. Runs `fio` against the
# same on-disk file at multiple read_ahead_kb settings and compares bandwidth.
#
# Two workloads are available (choose with WORKLOAD=):
#
#   WORKLOAD=scan     (default) Parallel mmap SEQUENTIAL-read streams over a
#                     file LARGER than RAM. Each 4 KiB fault looks sequential to
#                     the kernel, so on-demand read-ahead grows the window toward
#                     ra_pages (e.g. 4096 KiB). The huge prefetch churns the page
#                     cache and grossly over-reads unused pages -> throughput drops
#                     at large RA, and apg's step-down to 128 KiB helps.
#                     THIS is the RavenDB-style workload apg is built to fix.
#
#   WORKLOAD=randread Negative control. Pure random 4 KiB mmap reads. The kernel's
#                     adaptive read-ahead refuses to prefetch for random access,
#                     so RA makes no difference -> apg correctly stays idle.
#                     Kept to prove apg does NOT false-positive on genuine random I/O.
#
# Runs three phases per workload:
#   1. current   -- whatever the device is set to now (often 8 MiB / 4096 KiB)
#   2. baseline  -- read_ahead_kb = 4096  (problematic kernel default)
#   3. remediated- read_ahead_kb = 128   (apg's MIN_RA_KB floor)
#
# Usage:
#   sudo ./eval.sh /dev/sda /mnt/test           # scan workload (default)
#   sudo WORKLOAD=randread ./eval.sh /dev/sda /mnt/test
#
# Environment overrides:
#   WORKLOAD=scan|randread   FIO_RUNTIME_S=30   FIO_SIZE=6G
#   FIO_BS=4k                FIO_NUMJOBS=16
#   RESULTS_DIR=./results
#
# Requirements:
#   - fio (apt install fio)
#   - jq  (apt install jq)
#   - root (for sysfs writes and /proc/sys/vm/drop_caches)
#   - a writable mount point with at least FIO_SIZE free

set -euo pipefail

DEVICE="${1:-}"
TARGET_DIR="${2:-}"
RESULTS_DIR="${RESULTS_DIR:-./results}"
FIO_RUNTIME_S="${FIO_RUNTIME_S:-30}"
FIO_SIZE="${FIO_SIZE:-6G}"
FIO_BS="${FIO_BS:-4k}"
FIO_NUMJOBS="${FIO_NUMJOBS:-16}"
WORKLOAD="${WORKLOAD:-scan}"
FIO_FILE_NAME="${FIO_FILE_NAME:-fio_mmap_test}"

case "$WORKLOAD" in
    scan|randread) ;;
    *) echo "Invalid WORKLOAD='$WORKLOAD' (choose scan or randread)" >&2; exit 1 ;;
esac

bold()    { printf '\033[1m%s\033[0m\n' "$*"; }
info()    { printf '\033[0;36m[INFO ] %s\033[0m\n' "$*"; }
warn()    { printf '\033[0;33m[WARN ] %s\033[0m\n' "$*"; }
good()    { printf '\033[0;32m[ OK  ] %s\033[0m\n' "$*"; }
fail()    { printf '\033[0;31m[FAIL ] %s\033[0m\n' "$*" >&2; exit 1; }

# ---- Preflight checks ----
[[ -n "$DEVICE"     ]] || { echo "Usage: $0 <device> <target_dir>"; exit 1; }
[[ -n "$TARGET_DIR" ]] || { echo "Usage: $0 <device> <target_dir>"; exit 1; }

command -v fio >/dev/null 2>&1 || fail "fio not installed (apt install fio)"
command -v jq  >/dev/null 2>&1 || fail "jq not installed (apt install jq)"

# Accept either /dev/sda or sda. We only need the basename for sysfs.
DEVICE_NAME="$(basename "$DEVICE")"
SYSFS_RA="/sys/block/${DEVICE_NAME}/queue/read_ahead_kb"
[[ -w "$SYSFS_RA" ]] || fail "Cannot write $SYSFS_RA (run as root?)"

[[ -d "$TARGET_DIR" ]] || fail "Target dir $TARGET_DIR does not exist"
touch "$TARGET_DIR/.apg_eval_probe" 2>/dev/null || fail "Cannot write to $TARGET_DIR"
rm -f "$TARGET_DIR/.apg_eval_probe"

# Free space check (at least the fio file size + headroom; 6G file needs ~9G free)
free_kb=$(df -P "$TARGET_DIR" | awk 'NR==2 {print $4}')
need_kb=$(numfmt --from=auto "$FIO_SIZE")
need_kb=$(( need_kb / 1024 * 3 / 2 ))
[[ "$free_kb" -ge "$need_kb" ]] || fail "Need ${need_kb} KiB free in $TARGET_DIR, have ${free_kb} KiB"

mkdir -p "$RESULTS_DIR"
JOB_FILE="$(mktemp --suffix=.fio)"
# Capture the device's current read-ahead so the EXIT trap can restore it
# no matter how the script terminates (Ctrl-C, error, normal exit).
ORIGINAL_RA=$(cat "$SYSFS_RA" 2>/dev/null || echo 128)
info "Original read_ahead_kb on $DEVICE_NAME: $ORIGINAL_RA"
info "Workload: $WORKLOAD"
trap 'rm -f "$JOB_FILE"; echo "$ORIGINAL_RA" | tee "$SYSFS_RA" >/dev/null' EXIT

# ---- Generate fio job file ----
if [[ "$WORKLOAD" == "scan" ]]; then
    # Each job scans a disjoint contiguous region sequentially (offset_increment).
    # Total dataset spans FIO_SIZE, which MUST exceed RAM for cache churn to show.
    per_job_size=$(numfmt --to=iec "$(( $(numfmt --from=iec "$FIO_SIZE") / FIO_NUMJOBS ))")
    info "Scan workload: ${FIO_NUMJOBS} jobs x ${per_job_size}, file ${FIO_SIZE} (>RAM for cache churn)"
    cat > "$JOB_FILE" <<EOF
[global]
ioengine=mmap
rw=read
bs=${FIO_BS}
size=${per_job_size}
directory=${TARGET_DIR}
filename=${FIO_FILE_NAME}
runtime=${FIO_RUNTIME_S}
time_based=1
numjobs=${FIO_NUMJOBS}
group_reporting=1
direct=0
offset_increment=${per_job_size}

[scan_mmap]
stonewall
EOF
else
    info "Randread workload: pure random 4K mmap (negative control)"
    cat > "$JOB_FILE" <<EOF
[global]
ioengine=mmap
rw=randread
bs=${FIO_BS}
size=${FIO_SIZE}
directory=${TARGET_DIR}
filename=${FIO_FILE_NAME}
runtime=${FIO_RUNTIME_S}
time_based=1
numjobs=${FIO_NUMJOBS}
group_reporting=1
direct=0

[randread_mmap]
stonewall
EOF
fi

# ---- Helpers ----
set_ra()   { echo "$1" | tee "$SYSFS_RA" >/dev/null; }
drop_cache() {
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || warn "drop_caches failed"
    sleep 1
}

extract_metric() {
    local file="$1" key="$2"
    jq -r "$key // \"n/a\"" "$file" 2>/dev/null || echo "n/a"
}

# Sector-reads for the device (field 6 in /proc/diskstats). Works for both the
# whole disk (sda) and a partition (sda4), whichever has the active counters.
device_sectors() {
    awk -v d="$DEVICE_NAME" '$3==d{print $6; exit}' /proc/diskstats
}
major_faults() {
    awk '$1=="pgmajfault"{print $2}' /proc/vmstat
}
iowait_sample() {
    # Aggregate CPU iowait jiffies over a window; caller computes the delta %.
    awk '/^cpu  /{print $6}' /proc/stat
}
cpu_total() {
    awk '/^cpu  /{s=0; for(i=2;i<=NF;i++) s+=$i; print s}' /proc/stat
}

run_test() {
    local label="$1" ra_kb="$2" out
    out="$RESULTS_DIR/${label}.json"
    info "Running $label (read_ahead_kb=$ra_kb)"
    set_ra "$ra_kb"
    drop_cache
    local s0 f0 iw0 tot0
    s0=$(device_sectors); f0=$(major_faults)
    iw0=$(iowait_sample); tot0=$(cpu_total)
    local start_ts end_ts elapsed
    start_ts=$(date +%s.%N)
    fio --output-format=json --output="$out" "$JOB_FILE" 2>&1 \
        | sed 's/^/    fio: /'
    end_ts=$(date +%s.%N)
    elapsed=$(awk -v s="$start_ts" -v e="$end_ts" 'BEGIN {printf "%.2f", e-s}')
    local s1 f1 iw1 tot1 sect maj
    s1=$(device_sectors); f1=$(major_faults)
    iw1=$(iowait_sample); tot1=$(cpu_total)
    sect=$(( s1 - s0 )); maj=$(( f1 - f0 ))
    # Our own observations, bucketed, for later comparison.
    jq --arg ra "$ra_kb" --arg wall "$elapsed" \
       --argjson sect "$sect" --argjson maj "$maj" \
       --arg iow "$(awk -v a="$iw0" -v z="$iw1" -v t0="$tot0" -v t1="$tot1" \
                    'BEGIN{x= (t1>t0) ? (z-a)*100/(t1-t0) : 0; printf "%.1f", x}')" \
       '. + {apg_label: $ra, apg_wall_s: $wall, apg_sectors: $sect,
             apg_majfaults: $maj, apg_iowait_pct: $iow}' "$out" > "$out.tmp" \
        && mv "$out.tmp" "$out"
    good "Done $label (wall=${elapsed}s, sectors=${sect}, majfaults=${maj})"
}

# ---- Create the test file ----
# fio 3.x has no --quiet flag; use --output to /dev/null to silence its stats.
info "Pre-allocating test file (${FIO_SIZE}) in $TARGET_DIR ..."
fio --name=prep --filename="$TARGET_DIR/$FIO_FILE_NAME" \
    --ioengine=libaio --rw=write --bs=1M --size="$FIO_SIZE" \
    --iodepth=4 --direct=1 --group_reporting --output=/dev/null \
    && good "pre-allocated ${FIO_SIZE} test file" \
    || warn "prep failed; fio will allocate on the fly"

# ---- Phase 1: current setting (often worse than the kernel default) ----
bold "=== Phase 1: CURRENT (read_ahead_kb=$ORIGINAL_RA) ==="
run_test "current_${ORIGINAL_RA}KB" "$ORIGINAL_RA"

# ---- Phase 2: baseline (problematic 4 MiB kernel default) ----
bold "=== Phase 2: BASELINE (read_ahead_kb=4096) ==="
run_test "baseline_4MB" 4096

# ---- Phase 3: remediated (apg's MIN_RA_KB floor) ----
bold "=== Phase 3: REMEDIATED (read_ahead_kb=128) ==="
run_test "remediation_128KB" 128

# ---- Summary table ----
echo
bold "=== Summary ($WORKLOAD workload) ==="
printf '%-26s %-12s %-14s %-12s %-12s %-8s\n' \
       "label" "ra_kb" "bw_KiBps" "majfaults" "iowait_pct" "wall_s"
printf '%-26s %-12s %-14s %-12s %-12s %-8s\n' \
       "------------------------" "------------" "--------------" \
       "------------" "------------" "--------"

phase_files=("$RESULTS_DIR"/current_*.json "$RESULTS_DIR/baseline_4MB.json" "$RESULTS_DIR/remediation_128KB.json")
for f in "${phase_files[@]}"; do
    [[ -f "$f" ]] || continue
    label=$(basename "$f" .json)
    ra=$(extract_metric "$f" '.apg_label')
    bw=$(extract_metric "$f" '.jobs[0].read.bw')
    mj=$(extract_metric "$f" '.apg_majfaults')
    iw=$(extract_metric "$f" '.apg_iowait_pct // .jobs[0].read.iowait')
    wl=$(extract_metric "$f" '.apg_wall_s')
    printf '%-26s %-12s %-14s %-12s %-12s %-8s\n' "$label" "$ra" "$bw" "$mj" "$iw" "$wl"
done

echo
info "Raw JSON: $RESULTS_DIR/*.json (baseline_4MB / remediation_128KB)"
info "Sysfs read_ahead_kb restored to original value ($ORIGINAL_RA) by the EXIT trap."

# ---- Compute speedups ----
b_bw=$(extract_metric "$RESULTS_DIR/baseline_4MB.json"      '.jobs[0].read.bw')
r_bw=$(extract_metric "$RESULTS_DIR/remediation_128KB.json" '.jobs[0].read.bw')
if [[ "$b_bw" != "n/a" && "$r_bw" != "n/a" && "$b_bw" -gt 0 ]]; then
    speedup=$(awk -v b="$b_bw" -v r="$r_bw" 'BEGIN {printf "%.2fx", r/b}')
    bold "Throughput speedup (remediation / baseline): $speedup"
fi

for cur in "$RESULTS_DIR"/current_*.json; do
    [[ -f "$cur" ]] || continue
    c_bw=$(extract_metric "$cur" '.jobs[0].read.bw')
    if [[ "$c_bw" != "n/a" && "$r_bw" != "n/a" && "$c_bw" -gt 0 ]]; then
        speedup_from_current=$(awk -v c="$c_bw" -v r="$r_bw" 'BEGIN {printf "%.2fx", r/c}')
        bold "Throughput speedup (remediation / current): $speedup_from_current"
    fi
    break
done