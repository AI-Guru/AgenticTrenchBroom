#!/usr/bin/env bash
# Runs the MCP agent evaluation: starts TrenchBroom with the MCP server, lets an agent
# (Claude Code by default) work on task.md, then grades the result with grade.py.
#
#   app/TrenchBroom/test/mcp_eval/run_eval.sh <TrenchBroom binary | TrenchBroom.app> [out dir]
#
# Environment:
#   CLAUDE        the claude executable (default: claude)
#   CLAUDE_ARGS   extra arguments for claude, e.g. "--model sonnet"
#   AGENT=oracle  run oracle.py instead of an agent (checks this script and the grader)
#
# The out dir (default: a new temp dir) receives eval.map, transcript.jsonl, report.json
# and trenchbroom.log. TrenchBroom runs in portable mode; the portable config directory
# next to the binary is moved aside and restored afterwards. Exits with grade.py's status
# (0 = score 100).

set -euo pipefail

# don't leave __pycache__ directories in the source tree
export PYTHONDONTWRITEBYTECODE=1

if [[ $# -lt 1 ]]; then
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP="$1"
OUT="${2:-$(mktemp -d "${TMPDIR:-/tmp}/tb-mcp-eval.XXXXXX")}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"

if [[ -d "$APP" && "$APP" == *.app ]]; then
  BIN="$APP/Contents/MacOS/$(basename "${APP%.app}")"
else
  BIN="$APP"
fi
if [[ ! -x "$BIN" ]]; then
  echo "no such binary: $BIN" >&2
  exit 2
fi
BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

CONFIG_DIR="$(dirname "$BIN")/config"
BACKUP_DIR="$CONFIG_DIR.mcp-eval-backup-$$"
WORK_DIR="$(mktemp -d "${TMPDIR:-/tmp}/tb-mcp-eval-work.XXXXXX")"
TB_PID=""
MOVED_CONFIG=0

cleanup() {
  if [[ -n "$TB_PID" ]] && kill -0 "$TB_PID" 2>/dev/null; then
    kill -TERM "$TB_PID" 2>/dev/null || true
    for _ in $(seq 1 40); do
      kill -0 "$TB_PID" 2>/dev/null || break
      sleep 0.25
    done
    kill -KILL "$TB_PID" 2>/dev/null || true
  fi
  rm -rf "$CONFIG_DIR"
  if [[ "$MOVED_CONFIG" == 1 ]]; then
    mv "$BACKUP_DIR" "$CONFIG_DIR"
  fi
  rm -rf "$WORK_DIR"
}
trap cleanup EXIT

# a fresh portable config: no update prompts, MCP preferences off (--mcp-port starts it)
if [[ -e "$CONFIG_DIR" ]]; then
  mv "$CONFIG_DIR" "$BACKUP_DIR"
  MOVED_CONFIG=1
fi
mkdir -p "$CONFIG_DIR"
cat > "$CONFIG_DIR/Preferences.json" <<'EOF'
{"updater/Ask for auto updates": false, "updater/Check for updates automatically": false}
EOF

PORT="$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"

ENV_PREFIX=()
if [[ "$(uname)" == Linux && -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
  ENV_PREFIX=(env QT_QPA_PLATFORM=offscreen)
fi
(cd "$WORK_DIR" && exec ${ENV_PREFIX[@]+"${ENV_PREFIX[@]}"} "$BIN" --portable "--mcp-port=$PORT") \
  > "$OUT/trenchbroom.log" 2>&1 &
TB_PID=$!
# no "Terminated" job message when it is stopped
disown "$TB_PID"
echo "started TrenchBroom (pid $TB_PID) on http://127.0.0.1:$PORT/mcp; output in $OUT"

# wait until the server answers
HERE="$HERE" python3 -c '
import os, sys, time
sys.path.insert(0, os.path.join(os.environ["HERE"], ".."))
from mcp_e2e import Mcp
port, pid = int(sys.argv[1]), int(sys.argv[2])
deadline = time.time() + 60
while time.time() < deadline:
    try:
        os.kill(pid, 0)
    except OSError:
        sys.exit("TrenchBroom exited early")
    try:
        mcp = Mcp(port)
        mcp.open_session()
        mcp.close()
        sys.exit(0)
    except Exception:
        time.sleep(0.25)
sys.exit("the MCP server did not answer within 60 s")
' "$PORT" "$TB_PID"

if [[ "${AGENT:-claude}" == oracle ]]; then
  python3 "$HERE/oracle.py" --port "$PORT" --out "$OUT"
  : > "$OUT/transcript.jsonl"
else
  PROMPT="$(sed "s#<OUT>#$OUT#g" "$HERE/task.md")"
  MCP_CONFIG='{"mcpServers":{"trenchbroom":{"type":"http","url":"http://127.0.0.1:'"$PORT"'/mcp"}}}'
  # --allowedTools only pre-approves tools; the built-in tools are disallowed so that
  # the agent works and verifies through MCP only (ToolSearch stays, it loads MCP tools)
  # shellcheck disable=SC2086 # CLAUDE_ARGS is split on purpose
  "${CLAUDE:-claude}" -p "$PROMPT" \
    --mcp-config "$MCP_CONFIG" --strict-mcp-config \
    --allowedTools 'mcp__trenchbroom__*' \
    --disallowedTools 'Bash,Read,Write,Edit,MultiEdit,NotebookEdit,Glob,Grep,WebFetch,WebSearch,Task' \
    --output-format stream-json --verbose ${CLAUDE_ARGS:-} \
    > "$OUT/transcript.jsonl" || echo "the agent exited with status $?" >&2
fi

STATUS=0
python3 "$HERE/grade.py" --port "$PORT" --map "$OUT/eval.map" \
  --transcript "$OUT/transcript.jsonl" --pid "$TB_PID" --crash-dir "$WORK_DIR" \
  > "$OUT/report.json" || STATUS=$?
cat "$OUT/report.json"
echo "score: $(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["score"])' "$OUT/report.json")/100; files in $OUT"
exit "$STATUS"
