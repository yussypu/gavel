#!/usr/bin/env bash
# Streams a NASDAQ TotalView-ITCH sample day through gavel-extract; the gzip is decompressed in flight so nothing large touches disk.
set -euo pipefail

DAY="${DAY:-01302020}"
SYMBOLS="${SYMBOLS:-AAPL,MSFT,INTC,CSCO,TSLA}"
OUT="${OUT:-data/${DAY}}"
URL="https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/${DAY}.NASDAQ_ITCH50.gz"
BIN="${BIN:-build/tools/gavel-extract}"

if [ ! -x "${BIN}" ]; then
  echo "build gavel-extract first: ninja -C build gavel-extract" >&2
  exit 1
fi

mkdir -p "${OUT}"
echo "streaming ${URL}" >&2
curl -s --fail "${URL}" | gunzip -c | "${BIN}" --symbols "${SYMBOLS}" --out "${OUT}"
echo "done; outputs in ${OUT}" >&2
