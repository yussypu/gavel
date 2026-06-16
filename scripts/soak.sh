#!/usr/bin/env bash
# Bounded DST soak in three tiers (A: check-every-1 plus snapshot, B: check-every-1000, C: bulk), event counts sized per preset.
set -uo pipefail
BIN="${BIN:-build/tools/gavel-sim}"
LOG="${1:-soak.log}"
: > "$LOG"
fail=0
total=0

run() {
  desc="$1"; shift
  if out=$("$BIN" "$@" 2>&1); then
    echo "OK   $desc :: $out" >> "$LOG"
    n=$(printf '%s\n' "$out" | grep -oE 'msgs=[0-9]+' | head -1 | cut -d= -f2)
    [ -n "$n" ] && total=$((total + n))
  else
    echo "FAIL $desc :: $out" >> "$LOG"; fail=1
  fi
}

a_events() {
  case "$1" in
    default) echo 150000 ;;
    stop_cascade) echo 400000 ;;
    iceberg_sweep) echo 250000 ;;
    auction_mix) echo 80000 ;;
    tight_book) echo 120000 ;;
    peg_heavy) echo 40000 ;;
  esac
}

b_events() {
  case "$1" in
    default) echo 3000000 ;;
    stop_cascade) echo 6000000 ;;
    iceberg_sweep) echo 4000000 ;;
    auction_mix) echo 1500000 ;;
    tight_book) echo 800000 ;;
    peg_heavy) echo 120000 ;;
  esac
}

c_events() {
  case "$1" in
    default) echo 30000000 ;;
    stop_cascade) echo 50000000 ;;
    iceberg_sweep) echo 30000000 ;;
  esac
}

# Tier A: check-every-1 plus snapshot equality.
for preset in default stop_cascade iceberg_sweep auction_mix tight_book peg_heavy; do
  ev=$(a_events "$preset")
  for seed in 1 2 3; do
    run "A check1 $preset seed=$seed" --seed "$seed" --events "$ev" \
        --preset "$preset" --check-every 1 --snapshot-test
  done
done

# Tier B: check-every-1000 over millions.
for preset in default stop_cascade iceberg_sweep auction_mix tight_book peg_heavy; do
  ev=$(b_events "$preset")
  for seed in 4 5; do
    run "B check1k $preset seed=$seed" --seed "$seed" --events "$ev" \
        --preset "$preset" --check-every 1000
  done
done

# Tier C: bulk volume on bounded-live presets.
for preset in default stop_cascade iceberg_sweep; do
  ev=$(c_events "$preset")
  for seed in 11 12; do
    run "C bulk $preset seed=$seed" --seed "$seed" --events "$ev" \
        --preset "$preset" --check-every 50000
  done
done

echo "SOAK DONE fail=$fail total_events=$total" >> "$LOG"
