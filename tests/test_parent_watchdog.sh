#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BINARY="${ROOT}/build/c/codebase-memory-mcp"

case "$(uname -s)" in
  MINGW*|MSYS*|CYGWIN*)
    echo "skipping parent watchdog test on Windows"
    exit 0
    ;;
esac

if [[ ! -x "${BINARY}" ]]; then
  echo "missing binary: ${BINARY}" >&2
  exit 2
fi

tmpdir="$(mktemp -d)"
cleanup() {
  if [[ -f "${tmpdir}/child.pid" ]]; then
    child_pid="$(cat "${tmpdir}/child.pid" 2>/dev/null || true)"
    if [[ -n "${child_pid}" ]]; then
      kill "${child_pid}" 2>/dev/null || true
    fi
  fi
  if [[ -n "${wrapper_pid:-}" ]]; then
    kill "${wrapper_pid}" 2>/dev/null || true
  fi
  rm -rf "${tmpdir}"
}
trap cleanup EXIT

cat >"${tmpdir}/wrapper.sh" <<'SH'
#!/usr/bin/env bash
set -euo pipefail
exec 3<>"${FIFO}"
"${CBM_BINARY}" <&3 >/dev/null 2>"${TMPDIR_PATH}/child.err" &
echo "$!" >"${TMPDIR_PATH}/child.pid"
wait
SH
chmod +x "${tmpdir}/wrapper.sh"
mkfifo "${tmpdir}/stdin"

CBM_BINARY="${BINARY}" FIFO="${tmpdir}/stdin" TMPDIR_PATH="${tmpdir}" "${tmpdir}/wrapper.sh" &
wrapper_pid=$!

for _ in {1..50}; do
  [[ -s "${tmpdir}/child.pid" ]] && break
  sleep 0.1
done

if [[ ! -s "${tmpdir}/child.pid" ]]; then
  echo "child pid file was not written" >&2
  if [[ -s "${tmpdir}/child.err" ]]; then
    cat "${tmpdir}/child.err" >&2
  fi
  exit 3
fi

child_pid="$(cat "${tmpdir}/child.pid")"
if ! kill -0 "${child_pid}" 2>/dev/null; then
  echo "child did not start" >&2
  exit 3
fi

kill -9 "${wrapper_pid}"
wait "${wrapper_pid}" 2>/dev/null || true

deadline=$((SECONDS + 6))
while (( SECONDS < deadline )); do
  if ! kill -0 "${child_pid}" 2>/dev/null; then
    exit 0
  fi
  sleep 0.2
done

echo "codebase-memory-mcp child ${child_pid} survived parent death" >&2
if [[ -s "${tmpdir}/child.err" ]]; then
  cat "${tmpdir}/child.err" >&2
fi
exit 1
