#!/usr/bin/env bash
# Apply optional patches to ESP-IDF under ${IDF_PATH} (NimBLE ble_gap addr bypass, ble_sm pairing rsp callback).
# ble_sm changes are split into part1–part3 so GNU patch can apply hunks sequentially (offsets after each step).
# Safe to run multiple times: skips if already applied or context does not match this IDF revision.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PATCHES=(
  "${ROOT}/patches/nimble-ble-gap-bypass-random-addr.patch"
  "${ROOT}/patches/nimble-ble-sm-pairing-rsp-callback-part1.patch"
  "${ROOT}/patches/nimble-ble-sm-pairing-rsp-callback-part2.patch"
  "${ROOT}/patches/nimble-ble-sm-pairing-rsp-callback-part3.patch"
)

if [[ -z "${IDF_PATH:-}" ]]; then
  echo "apply-idf-patches: IDF_PATH not set, skipping"
  exit 0
fi

apply_one() {
  local PATCH="$1"
  local name
  name="$(basename "${PATCH}")"

  if [[ ! -f "${PATCH}" ]]; then
    echo "apply-idf-patches: missing ${name}, skip"
    return 0
  fi

  if patch -p1 --dry-run -d "${IDF_PATH}" -i "${PATCH}" >/dev/null 2>&1; then
    patch -p1 -d "${IDF_PATH}" -i "${PATCH}"
    echo "apply-idf-patches: applied ${name}"
  elif patch -p1 -R --dry-run -d "${IDF_PATH}" -i "${PATCH}" >/dev/null 2>&1; then
    echo "apply-idf-patches: ${name} already applied (skip)"
  else
    echo "apply-idf-patches: WARNING — ${name} does not match this IDF tree; skip"
  fi
}

for p in "${PATCHES[@]}"; do
  apply_one "$p"
done
