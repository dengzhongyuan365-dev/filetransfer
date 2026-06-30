#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT_DIR}/build"
SENDER="${BUILD_DIR}/src/sender"
RECEIVER="${BUILD_DIR}/src/receiver"

TMP_DIR="$(mktemp -d /tmp/filetransfer-manual.XXXXXX)"
PORT="$((30000 + RANDOM % 20000))"
RECEIVER_PID=""

cleanup() {
    if [[ -n "${RECEIVER_PID}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

log() {
    printf '[manual-verify] %s\n' "$*"
}

fail() {
    printf '[manual-verify] failed: %s\n' "$*" >&2
    exit 1
}

wait_for_receiver() {
    local log_file="$1"
    for _ in {1..50}; do
        if grep -q 'listening for transfers' "${log_file}"; then
            return 0
        fi
        sleep 0.1
    done
    fail "receiver did not start; log: $(cat "${log_file}" 2>/dev/null || true)"
}

stop_receiver() {
    if [[ -n "${RECEIVER_PID}" ]] && kill -0 "${RECEIVER_PID}" 2>/dev/null; then
        kill "${RECEIVER_PID}" 2>/dev/null || true
        wait "${RECEIVER_PID}" 2>/dev/null || true
    fi
    RECEIVER_PID=""
}

start_receiver() {
    local port="$1"
    local log_file="$2"
    shift 2

    "${RECEIVER}" --bind 127.0.0.1 --port "${port}" --dir "${RECEIVE_DIR}" "$@" >"${log_file}" 2>&1 &
    RECEIVER_PID="$!"
    wait_for_receiver "${log_file}"
}

log "building project"
cmake --build "${BUILD_DIR}" >/dev/null

[[ -x "${SENDER}" ]] || fail "missing sender executable at ${SENDER}"
[[ -x "${RECEIVER}" ]] || fail "missing receiver executable at ${RECEIVER}"

SOURCE_DIR="${TMP_DIR}/source"
RECEIVE_DIR="${TMP_DIR}/receive"
mkdir -p "${SOURCE_DIR}" "${RECEIVE_DIR}"

RECEIVER_LOG="${TMP_DIR}/receiver.log"
log "starting receiver without overwrite on 127.0.0.1:${PORT}"
start_receiver "${PORT}" "${RECEIVER_LOG}"

log "case 1: normal file transfer"
printf 'alpha\nbeta\ngamma\n' >"${SOURCE_DIR}/normal.txt"
"${SENDER}" --host 127.0.0.1 --port "${PORT}" --path "${SOURCE_DIR}/normal.txt" >/tmp/filetransfer-manual-send-normal.log 2>&1
cmp "${SOURCE_DIR}/normal.txt" "${RECEIVE_DIR}/normal.txt"

log "case 2: identical target is skipped"
SKIP_OUTPUT="$("${SENDER}" --host 127.0.0.1 --port "${PORT}" --path "${SOURCE_DIR}/normal.txt" 2>&1)"
printf '%s\n' "${SKIP_OUTPUT}" | grep -q 'skipped identical file' || {
    printf '%s\n' "${SKIP_OUTPUT}" >&2
    fail "sender did not report skipped identical file"
}

log "case 3: different target is rejected without overwrite"
printf 'new content\n' >"${SOURCE_DIR}/conflict.txt"
printf 'old content\n' >"${RECEIVE_DIR}/conflict.txt"
set +e
CONFLICT_OUTPUT="$("${SENDER}" --host 127.0.0.1 --port "${PORT}" --path "${SOURCE_DIR}/conflict.txt" 2>&1)"
CONFLICT_STATUS="$?"
set -e
[[ "${CONFLICT_STATUS}" -ne 0 ]] || fail "conflict transfer unexpectedly succeeded"
printf '%s\n' "${CONFLICT_OUTPUT}" | grep -q 'target already exists' || {
    printf '%s\n' "${CONFLICT_OUTPUT}" >&2
    fail "conflict transfer did not report target already exists"
}
grep -q 'old content' "${RECEIVE_DIR}/conflict.txt" || fail "conflict target was modified"

log "case 4: overwrite different target"
stop_receiver
PORT="$((PORT + 1))"
RECEIVER_LOG="${TMP_DIR}/receiver-overwrite.log"
log "starting receiver with overwrite on 127.0.0.1:${PORT}"
start_receiver "${PORT}" "${RECEIVER_LOG}" --allow-overwrite
OVERWRITE_OUTPUT="$("${SENDER}" --host 127.0.0.1 --port "${PORT}" --path "${SOURCE_DIR}/conflict.txt" 2>&1)"
printf '%s\n' "${OVERWRITE_OUTPUT}" | grep -q 'sent file' || {
    printf '%s\n' "${OVERWRITE_OUTPUT}" >&2
    fail "overwrite transfer did not report sent file"
}
cmp "${SOURCE_DIR}/conflict.txt" "${RECEIVE_DIR}/conflict.txt"

log "case 5: resume from existing part file"
printf 'abcdef\n' >"${SOURCE_DIR}/resume.txt"
printf 'abc' >"${RECEIVE_DIR}/resume.txt.part"
RESUME_OUTPUT="$("${SENDER}" --host 127.0.0.1 --port "${PORT}" --path "${SOURCE_DIR}/resume.txt" --chunk-size 3B 2>&1)"
printf '%s\n' "${RESUME_OUTPUT}" | grep -q 'resumed file' || {
    printf '%s\n' "${RESUME_OUTPUT}" >&2
    fail "resume transfer did not report resumed file"
}
cmp "${SOURCE_DIR}/resume.txt" "${RECEIVE_DIR}/resume.txt"
[[ ! -e "${RECEIVE_DIR}/resume.txt.part" ]] || fail "resume part file was left behind"

log "all manual verification cases passed"
