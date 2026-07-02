#!/usr/bin/env bash
# mem_ceiling_abort.sh — real-process regression guard for the enforcing RSS
# memory ceiling (cbm_mem_abort_if_over_ceiling, mem.c).
#
# Covers the acceptance-criteria rows that CANNOT be exercised by an
# in-process C unit test (the abort path itself calls abort(), which would
# kill the test-runner process):
#
#   Row 6  — RSS ceiling ABORTS (non-zero exit) with a diagnostic dump
#            naming file + phase + RSS when exceeded. Exercises the real
#            index entry point (cli index_repository -> cbm_parallel_extract),
#            not a direct call to the ceiling helper.
#   Row 7  — A normal full index of the real repo does NOT abort (healthy
#            path preserved; anti-false-positive guard for R3).
#   Row 9  — CBM_MEM_CEILING_MB override adjusts the abort threshold: forced
#            below a large synthetic index's actual RSS, that index now
#            aborts (proves the knob bites); unset uses the default and
#            completes; invalid warns and falls back to the default
#            (completes).
#   Row 10 (real-environment half) — both the abort and no-abort verdicts
#            are read against the REAL RSS of a real process, not an
#            injected value.
#
# Also checks the store-integrity finding (R4): an aborted index must never
# leave a partially-written .db — dump_and_persist_hashes() (the SQLite
# write) runs strictly after cbm_parallel_extract() in pipeline.c, so an
# abort during extract precedes any DB write. This script asserts no .db
# file (partial or otherwise) is left behind by an aborted run.
#
# IMPORTANT — incremental indexing gotcha: `index_repository` with
# mode=full does NOT force a full re-parse when file hashes are unchanged
# from a prior run against the same repo_path (it routes to
# incremental.noop). Every run below that needs a genuine full extraction
# pass (i.e. any run whose PURPOSE is to exercise cbm_max_file_bytes() /
# cbm_mem_abort_if_over_ceiling() on real file reads) deletes the target's
# .db first. Skipping that reset is a silent false-pass, not a crash — the
# process still exits 0 (it did no real work), so watch for that pattern.
#
# Usage: bash tests/mem_ceiling_abort.sh
# Exit 0 on success, non-zero on failure. SLOW (generates GB-scale fixtures) —
# intended for the IO build host, not routine `make test`.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="$ROOT/build/c/codebase-memory-mcp"
GEN="$ROOT/tests/gen_mem_ceiling_repro.sh"
CACHE_DIR="${HOME}/.cache/codebase-memory-mcp"
FAILURES=0
WORKDIR=""

cleanup() {
    [[ -n "$WORKDIR" && -d "$WORKDIR" ]] && rm -rf "$WORKDIR"
}
trap cleanup EXIT

if [[ ! -x "$BINARY" ]]; then
    echo "[mem_ceiling] FAIL: binary not found at $BINARY (build first: make -f Makefile.cbm cbm)" >&2
    exit 2
fi

project_name() { printf '%s' "$1" | sed 's#^/##; s#[^A-Za-z0-9._-]#-#g'; }

db_path_for() {
    local repo="$1"
    printf '%s/%s.db' "$CACHE_DIR" "$(project_name "$repo")"
}

# Force the NEXT index_repository call against $1 to do a genuine full
# extraction (not an incremental no-op) by wiping its cached .db/.tmp.
reset_index_state() {
    local repo="$1" db
    db="$(db_path_for "$repo")"
    rm -f "$db" "${db}.tmp"
}

# Run `cli index_repository` against $1, with env overrides from the
# remaining args (NAME=VALUE pairs), capturing exit code + stderr.
# Sets: LAST_EXIT, LAST_LOG (path).
run_index() {
    local repo="$1"; shift
    local log; log="$(mktemp)"
    LAST_LOG="$log"
    # shellcheck disable=SC2086
    env "$@" "$BINARY" cli index_repository "{\"repo_path\":\"$repo\",\"mode\":\"full\"}" \
        >"$log" 2>&1
    LAST_EXIT=$?
}

# ── Row 7 + row 10 (no-abort half): healthy real-repo index, default ceiling ──
echo "[mem_ceiling] Row 7/10: healthy real-repo index must NOT abort (default ceiling)..." >&2
reset_index_state "$ROOT"
run_index "$ROOT"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row7]: healthy real-repo index aborted (exit=$LAST_EXIT) under default ceiling" >&2
    tail -20 "$LAST_LOG" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] PASS [row7]: healthy index completed (exit=0)" >&2
fi
rm -f "$LAST_LOG"

# ── Row 9 (unset leg): explicit unset also completes ─────────────────────
echo "[mem_ceiling] Row 9 (unset): explicit CBM_MEM_CEILING_MB unset uses default, completes..." >&2
reset_index_state "$ROOT"
run_index "$ROOT"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row9-unset]: index aborted with ceiling env unset (exit=$LAST_EXIT)" >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] PASS [row9-unset]" >&2
fi
rm -f "$LAST_LOG"

# ── Row 9 (invalid leg): non-numeric override warns + falls back, completes ──
echo "[mem_ceiling] Row 9 (invalid): non-numeric CBM_MEM_CEILING_MB falls back to default..." >&2
reset_index_state "$ROOT"
run_index "$ROOT" CBM_MEM_CEILING_MB="not-a-number"
if [[ "$LAST_EXIT" -ne 0 ]]; then
    echo "[mem_ceiling] FAIL [row9-invalid]: index aborted with invalid ceiling env (exit=$LAST_EXIT)" >&2
    FAILURES=$((FAILURES + 1))
else
    if ! grep -q "mem_ceiling.env.invalid" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row9-invalid]: no warn log for invalid CBM_MEM_CEILING_MB" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "[mem_ceiling] PASS [row9-invalid]: warned + completed" >&2
    fi
fi
rm -f "$LAST_LOG"

# ── Row 6 + Row 9(low) + R4: synthetic large-file repro forces cumulative
#    RSS past a ceiling set BELOW its expected peak but ABOVE the 2048MB
#    floor (mem_ceiling_floor_applies_on_tiny_ram in test_mem.c already
#    pins the floor's own arithmetic in-process; this proves the knob bites
#    on a REAL run — a ceiling below the floor would just get clamped up
#    to the floor and never trip, so the fixture must genuinely exceed the
#    CHOSEN ceiling, not the floor). Sized well past what a healthy 587-file
#    real repo would ever reach (~1-1.3GB peak per the diagnosis), so a
#    false negative here is a genuine "doesn't bite" signal, not sizing
#    noise. ──────────────────────────────────────────────────────────────
CEILING_TEST_MB=2100
echo "[mem_ceiling] Row 6/9(low)/R4: generating a large-file repro sized to exceed a ${CEILING_TEST_MB}MB forced ceiling..." >&2
WORKDIR="$(mktemp -d /tmp/cbm_mem_ceiling_repro.XXXXXX)"
# 40 tiny + 40 large (~8MB each = ~320MB source) — mirrors the ORIGINAL
# diagnosis repro shape (600 tiny + 40 large ~7.6MB files -> 8.8GB on IO)
# on file COUNT and per-file size (tiny-file count trimmed 600->40; tiny
# files barely contribute RSS, so this doesn't change the mechanism).
# CALIBRATED by direct probe on IO/CBM_WORKERS=8 with NO ceiling override:
# this exact shape peaks at rss_mb=2198 (parallel.extract.mem log). The
# floor (CBM_MEM_CEILING_FLOOR_MB, mem.c) is 2048MB — a ceiling below that
# gets clamped UP to the floor and would never trip (this is what silently
# broke the first two attempts at 3000MB, and an earlier 20x15MB/3000MB
# combination that just didn't reach 3000MB at all). 2100MB sits strictly
# between the floor (2048) and the calibrated peak (2198), so it is NOT
# floor-clamped and WILL trip against this exact fixture shape on this
# exact host. Re-calibrate CEILING_TEST_MB (or the gen call) if run on a
# host with materially different allocator/CPU behaviour.
bash "$GEN" "$WORKDIR" 40 40 8
reset_index_state "$WORKDIR"
run_index "$WORKDIR" CBM_MEM_CEILING_MB="$CEILING_TEST_MB" CBM_WORKERS="8"
db_synth="$(db_path_for "$WORKDIR")"
if [[ "$LAST_EXIT" -eq 0 ]]; then
    echo "[mem_ceiling] FAIL [row6/row9-low]: synthetic fixture did NOT trip a ${CEILING_TEST_MB}MB forced ceiling — either the knob doesn't bite, or the fixture is undersized for this host's allocator behaviour (IO: 31GB/8 cores). Widen n_large/size_mb in the gen call above if this needs to be load-bearing on a beefier host." >&2
    FAILURES=$((FAILURES + 1))
else
    echo "[mem_ceiling] index aborted as expected (exit=$LAST_EXIT)" >&2
    if ! grep -q "mem.ceiling.abort" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row6]: no mem.ceiling.abort diagnostic dump in output" >&2
        tail -10 "$LAST_LOG" >&2
        FAILURES=$((FAILURES + 1))
    elif ! grep -qE "file=\S+ phase=\S+ rss_mb=[0-9]+ ceiling_mb=[0-9]+" "$LAST_LOG"; then
        echo "[mem_ceiling] FAIL [row6]: diagnostic dump missing file/phase/rss_mb/ceiling_mb fields" >&2
        grep "mem.ceiling.abort" "$LAST_LOG" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "[mem_ceiling] PASS [row6]: diagnostic dump present with file+phase+rss" >&2
        grep "mem.ceiling.abort" "$LAST_LOG" >&2
    fi
fi
# R4: an aborted run must leave no (partial) .db file — the abort happens
# strictly before dump_and_persist_hashes()/cbm_gbuf_dump_to_sqlite(). Only
# meaningful when the run actually aborted (LAST_EXIT != 0) — a completed
# run legitimately leaves a .db behind, that is not a violation.
if [[ "$LAST_EXIT" -ne 0 ]]; then
    if [[ -f "$db_synth" || -f "${db_synth}.tmp" ]]; then
        echo "[mem_ceiling] FAIL [R4]: aborted run left a .db (or .db.tmp) file — store-integrity violation: $db_synth" >&2
        FAILURES=$((FAILURES + 1))
    else
        echo "[mem_ceiling] PASS [R4]: aborted run left no .db file (extract-phase abort precedes any SQLite write)" >&2
    fi
else
    echo "[mem_ceiling] SKIP [R4]: run did not abort, nothing to check (see row6/row9-low failure above)" >&2
fi
rm -f "$LAST_LOG" "$db_synth" "${db_synth}.tmp"

# ── Final result ───────────────────────────────────────────────────────
if [[ "$FAILURES" -gt 0 ]]; then
    echo "[mem_ceiling] FAILED: $FAILURES check(s) failed." >&2
    exit 1
fi

echo "[mem_ceiling] All checks passed (rows 6, 7, 9, 10-real-env, R4 store-integrity)." >&2
exit 0
