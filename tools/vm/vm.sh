#!/bin/bash
# Drive the PSDsoft XP VM (VirtualBox "winxp" on the lab PC) from the Mac.
#
#   tools/vm/vm.sh "click 143 290" "sleep 3000"      # then look at $OUT (vm.png)
#   tools/vm/vm.sh --run 'dir C:\PSDexpress\proj'   # a command in the guest, output printed
#   tools/vm/vm.sh --get zrp6v7.jam zrp6.frp         # copy from C:\PSDexpress\proj to $OUT_DIR
#   tools/vm/vm.sh --put local.hex                   # copy into C:\PSDexpress\proj
#
# Mouse input goes through VirtualBox's COM API (IMouse.PutMouseEventAbsolute,
# guest coordinates), keys through the same session, screenshots through
# VBoxManage — so nothing depends on the lab PC's own desktop or focus, and it
# works from an SSH session. vmctl.ps1 lists the commands.
#
# Credentials come from the environment, never from this file:
#   ZRP6_PC_HOST   default Admin@10.0.0.107 (the OPTIPLEX; .106 is something else now)
#   ZRP6_PC_PW     or the first line of ~/.zrp6_pc_pw
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
H="${ZRP6_PC_HOST:-Admin@10.0.0.107}"
PW="${ZRP6_PC_PW:-$(head -n1 "$HOME/.zrp6_pc_pw" 2>/dev/null)}"
OUT="${OUT:-vm.png}"
OUT_DIR="${OUT_DIR:-.}"
O="-o StrictHostKeyChecking=no -o ConnectTimeout=15"
W='C:\Users\Admin\Downloads\zrp6\vm'
VB='$vb="C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"'
GA='--username Administrator --password admin'
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

put() { local n; for n in 1 2 3 4; do sshpass -p "$PW" scp $O "$1" "$H:$2" >/dev/null 2>&1 && return 0; sleep 2; done; return 1; }
get() { local n; for n in 1 2 3; do sshpass -p "$PW" scp $O "$H:$1" "$2" >/dev/null 2>&1 && return 0; sleep 2; done; return 1; }
ps()  { put "$1" "C:/Users/Admin/$(basename "$1")" && sshpass -p "$PW" ssh $O "$H" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\Users\\Admin\\$(basename "$1")"; }

case "${1:-}" in
--run)
    { echo "$VB"; echo "New-Item -ItemType Directory -Force $W | Out-Null"
      echo "& \$vb guestcontrol winxp run --exe C:\\WINDOWS\\system32\\cmd.exe $GA -- cmd.exe /c \"$2 > C:\\out.txt 2>&1\" 2>&1 | Out-Null"
      echo "& \$vb guestcontrol winxp copyfrom $GA C:\\out.txt --target-directory $W\\ 2>&1 | Out-Null"
      echo "Get-Content $W\\out.txt"; } > "$TMP/vmrun.ps1"
    ps "$TMP/vmrun.ps1" ;;
--get)
    shift
    { echo "$VB"; for f in "$@"; do echo "& \$vb guestcontrol winxp copyfrom $GA \"C:\\PSDexpress\\proj\\$f\" --target-directory \"$W\\\" 2>&1 | Out-Null"; done; } > "$TMP/vmget.ps1"
    ps "$TMP/vmget.ps1" >/dev/null
    for f in "$@"; do get "C:/Users/Admin/Downloads/zrp6/vm/$f" "$OUT_DIR/$f" && echo "got $OUT_DIR/$f"; done ;;
--put)
    put "$2" "C:/Users/Admin/Downloads/zrp6/vm/$(basename "$2")"
    { echo "$VB"; echo "& \$vb guestcontrol winxp copyto $GA \"$W\\$(basename "$2")\" --target-directory \"C:\\PSDexpress\\proj\\\""; } > "$TMP/vmput.ps1"
    ps "$TMP/vmput.ps1" ;;
*)
    : > "$TMP/vmctl_cmds.txt"
    for c in "$@"; do printf '%s\r\n' "$c" >> "$TMP/vmctl_cmds.txt"; done
    put "$HERE/vmctl.ps1" C:/Users/Admin/vmctl.ps1 && put "$TMP/vmctl_cmds.txt" C:/Users/Admin/vmctl_cmds.txt
    sshpass -p "$PW" ssh $O "$H" "powershell -NoProfile -ExecutionPolicy Bypass -File C:\\Users\\Admin\\vmctl.ps1" 2>&1 | tail -3
    get C:/Users/Admin/vm.png "$OUT" && echo "screenshot: $OUT" ;;
esac
