#!/usr/bin/env bash
# eval.sh - Evaluation harness for Adaptive Prefetch Guard
#
# Demonstrates the page-cache thrashing regression caused by oversized block
# read-ahead on a memory-mapped random-read workload (the RavenDB / RocksDB
# access pattern from FR-02). Runs `fio` against the same on-disk file at
# multiple read-ahead settings:
#
#   1. current   -- whatever the device is set to right now (often 8 MiB)
#   2. baseline  -- read_ahead_kb = 4096  (problematic kernel default)
#   3. remediated- read_ahead_kb = 128   (apg's MIN_RA_KB floor)
#
# Compares read bandwidth, iowait, and total wall-clock runtime.
#
# Usage:
#   sudo ./eval.sh /dev/sda /mnt/test           # bare-metal
#   sudo ./eval.sh /dev/nvme0n1 /var/lib/db
#
# Requirements:
#   - fio (apt install fio)
#   - jq  (apt install jq)
#   - root (for sysfs writes and /proc/sys/vm/drop_caches)
#   - a writable mount point with at least 4 GiB free

set -euo pipefail

DEVICE="${1:-}"
TARGET_DIR="${2:-}"
RESULTS_DIR="${RESULTS_DIR:-./results}"
FIO_RUNTIME_S="${FIO_RUNTIME_S:-30}"
FIO_SIZE="${FIO_SIZE:-2G}"
FIO_BS="${FIO_BS:-4k}"
FIO_NUMJOBS="${FIO_NUMJOBS:-4}"
FIO_FILE_NAME="${FIO_FILE_NAME:-fio_mmap_test}"

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

# Free space check (at least the fio file size + headroom)
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
trap 'rm -f "$JOB_FILE"; echo "$ORIGINAL_RA" | tee "$SYSFS_RA" >/dev/null' EXIT

# ---- Generate fio job file ----
# ioengine=mmap + rw=randread is the canonical thrashing trigger:
# every major fault pulls in a full read-ahead block but the random
# access pattern means the prefetched pages are never reused.
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
iodepth=1
group_reporting=1
direct=0
mem=malloc
exitall=1

[randread_mmap]
stonewall
EOF

# ---- Helpers ----
get_ra()   { cat "$SYSFS_RA"; }
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

run_test() {
    local label="$1" ra_kb="$2" out
    out="$RESULTS_DIR/${label}.json"
    info "Running $label (read_ahead_kb=$ra_kb)"
    set_ra "$ra_kb"
    drop_cache
    local start_ts end_ts elapsed
    start_ts=$(date +%s.%N)
    fio --output-format=json --output="$out" "$JOB_FILE" 2>&1 \
        | sed 's/^/    fio: /'
    end_ts=$(date +%s.%N)
    elapsed=$(awk -v s="$start_ts" -v e="$end_ts" 'BEGIN {printf "%.2f", e-s}')
    # Augment the JSON with our own observations for easy comparison.
    jq --arg ra "$ra_kb" --arg wall "$elapsed" \
       '. + {apg_label: $ra, apg_wall_s: $wall}' "$out" > "$out.tmp" \
        && mv "$out.tmp" "$out"
    good "Done $label (wall=${elapsed}s)"
}

# ---- Optional: pre-create the test file with sequential write so the
#      random-read pass isn't dominated by initial allocation cost. ----
info "Pre-allocating test file (${FIO_SIZE}) in $TARGET_DIR ..."
fio --name=prep --filename="$TARGET_DIR/$FIO_FILE_NAME" \
    --ioengine=libaio --rw=write --bs=1M --size="$FIO_SIZE" \
    --iodepth=4 --direct=1 --group_reporting --quiet \
    --exitall=1 || warn "prep failed; fio will allocate on the fly"

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
bold "=== Summary ==="
printf '%-26s %-14s %-14s %-14s %-14s\n' \
       "label" "ra_kb" "bw_KiBps" "iowait_pct" "wall_s"
printf '%-26s %-14s %-14s %-14s %-14s\n' \
       "------------------------" "------------" "------------" \
       "------------" "------------"

# Dynamically discover which phase JSONs exist so the summary table
# works no matter how many phases ran (1, 2, or 3).
phase_files=("$RESULTS_DIR"/current_*.json "$RESULTS_DIR/baseline_4MB.json" "$RESULTS_DIR/remediation_128KB.json")
for f in "${phase_files[@]}"; do
    [[ -f "$f" ]] || continue
    label=$(basename "$f" .json)
    ra=$(extract_metric "$f" '.apg_label')
    bw=$(extract_metric "$f" '.jobs[0].read.bw')
    iw=$(extract_metric "$f" '.jobs[0].read.iowait')
    wl=$(extract_metric "$f" '.apg_wall_s')
    printf '%-26s %-14s %-14s %-14s %-14s\n' "$label" "$ra" "$bw" "$iw" "$wl"
done

echo
info "Raw JSON: $RESULTS_DIR/*.json"
info "Sysfs read_ahead_kb restored to original value ($ORIGINAL_RA) by the EXIT trap."

# ---- Compute speedups ----
b_bw=$(extract_metric "$RESULTS_DIR/baseline_4MB.json"      '.jobs[0].read.bw')
r_bw=$(extract_metric "$RESULTS_DIR/remediation_128KB.json" '.jobs[0].read.bw')
if [[ "$b_bw" != "n/a" && "$r_bw" != "n/a" && "$b_bw" -gt 0 ]]; then
    speedup=$(awk -v b="$b_bw" -v r="$r_bw" 'BEGIN {printf "%.2fx", r/b}')
    bold "Throughput speedup (remediation / baseline): $speedup"
fi

# Speedup vs the original (pre-benchmark) setting, if that phase ran.
for cur in "$RESULTS_DIR"/current_*.json; do
    [[ -f "$cur" ]] || continue
    c_bw=$(extract_metric "$cur" '.jobs[0].read.bw')
    if [[ "$c_bw" != "n/a" && "$r_bw" != "n/a" && "$c_bw" -gt 0 ]]; then
        speedup_from_current=$(awk -v c="$c_bw" -v r="$r_bw" 'BEGIN {printf "%.2fx", r/c}')
        bold "Throughput speedup (remediation / current): $speedup_from_current"
    fi
    break
done
