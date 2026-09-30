#!/bin/bash
# rc.sh -- drive the Windows PC's desktop from the Mac (screenshot + input).
#
# An SSH session on Windows runs in session 0 and cannot touch the interactive
# desktop -- CopyFromScreen fails with "The handle is invalid". So the actual
# work runs INSIDE the logged-on user's session via an interactive scheduled
# task (schtasks /IT), which is what ctl.ps1 / run_ctl.ps1 on the PC do.
#
#   tools/rc.sh                              # just screenshot
#   tools/rc.sh "click 570 120"              # one command
#   tools/rc.sh "click 570 120" "sleep 1200" "key {END}"
#
# Commands: click X Y | dblclick X Y | move X Y | type <text> | key <SendKeys>
#           | sleep <ms>
#
# The screenshot is fetched to $OUT (default: ./pc_screen.png) for viewing.
#
# Credentials come from the environment, NOT from this file:
#   ZRP6_PC_HOST  (default Admin@10.0.0.107)
#   ZRP6_PC_PW    required -- e.g. export ZRP6_PC_PW='...'
#                 or put it in ~/.zrp6_pc_pw (chmod 600)
set -u

PC="${ZRP6_PC_HOST:-Admin@10.0.0.107}"
if [ -z "${ZRP6_PC_PW:-}" ] && [ -r "$HOME/.zrp6_pc_pw" ]; then
    ZRP6_PC_PW="$(head -n1 "$HOME/.zrp6_pc_pw")"
fi
if [ -z "${ZRP6_PC_PW:-}" ]; then
    echo "rc.sh: set ZRP6_PC_PW, or put the password in ~/.zrp6_pc_pw" >&2
    exit 2
fi

OUT="${ZRP6_PC_SHOT:-pc_screen.png}"
TMP="$(mktemp -t zrp6rc)"
trap 'rm -f "$TMP"' EXIT

SSHOPT="-o StrictHostKeyChecking=no -o ConnectTimeout=15"

: > "$TMP"
for c in "$@"; do printf '%s\n' "$c" >> "$TMP"; done

# sshd on the PC intermittently refuses a connection when they come back to
# back, so every hop retries with a short backoff.
retry() {
    local n=0
    until "$@"; do
        n=$((n + 1))
        [ "$n" -ge 6 ] && return 1
        sleep $((n * 2))
    done
}

retry sshpass -p "$ZRP6_PC_PW" scp $SSHOPT "$TMP" "$PC:ctl_cmds.txt" >/dev/null 2>&1 \
    || { echo "rc.sh: failed to send commands" >&2; exit 1; }

retry sshpass -p "$ZRP6_PC_PW" ssh $SSHOPT "$PC" \
    "powershell -NoProfile -ExecutionPolicy Bypass -File run_ctl.ps1" 2>&1 | tail -14

# The screenshot copy occasionally races the task; retry briefly.
for i in 1 2 3; do
    if retry sshpass -p "$ZRP6_PC_PW" scp $SSHOPT "$PC:screen.png" "$OUT" >/dev/null 2>&1; then
        echo "screenshot: $OUT ($(wc -c < "$OUT" | tr -d ' ') bytes)"
        exit 0
    fi
    sleep 2
done
echo "rc.sh: could not fetch screenshot" >&2
exit 1
