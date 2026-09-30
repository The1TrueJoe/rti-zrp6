# vmctl.ps1 — drive the winxp VM: mouse through the VirtualBox COM API, keys
# and screenshots through VBoxManage. Commands, one per line, in vmctl_cmds.txt:
#   click X Y | dblclick X Y | rclick X Y | move X Y | drag X1 Y1 X2 Y2 | type TEXT | key NAME[,NAME...] | sleep MS
# key names: enter tab esc end home del bksp up down left right space f4 pgdn pgup
#            and combos like shift+home, ctrl+a, alt+f, alt+f4, shift+tab,
#            alt+minus (an MDI child's system menu)
$ErrorActionPreference = 'Stop'
$vbm = "C:\Program Files\Oracle\VirtualBox\VBoxManage.exe"
$client = New-Object -ComObject VirtualBox.VirtualBoxClient
$machine = $client.VirtualBox.FindMachine('winxp')
$session = $client.Session
$machine.LockMachine($session, 1)          # shared lock: the VM keeps running
$mouse = $session.Console.Mouse
$kb = $session.Console.Keyboard

$sc = @{ enter=0x1c; tab=0x0f; esc=0x01; space=0x39; bksp=0x0e; f4=0x3e; f2=0x3c; f5=0x3f
         a=0x1e; c=0x2e; v=0x2f; x=0x2d; f=0x21; s=0x1f; o=0x18; n=0x31; y=0x15; m=0x32; r=0x13; d=0x20
         minus=0x0c; g=0x22; e=0x12; t=0x14 }
$ext = @{ end=0x4f; home=0x47; del=0x53; up=0x48; down=0x50; left=0x4b; right=0x4d; pgdn=0x51; pgup=0x49 }
$mod = @{ shift=0x2a; ctrl=0x1d; alt=0x38 }

function Codes($name) {
    if ($ext.ContainsKey($name)) { return ,@(0xe0, $ext[$name]) }
    if ($sc.ContainsKey($name)) { return ,@($sc[$name]) }
    throw "unknown key $name"
}
function Put([int[]]$codes) { foreach ($c in $codes) { $kb.PutScancode($c) }; Start-Sleep -Milliseconds 40 }
function Key($combo) {
    $parts = $combo.Split('+')
    $mods = $parts[0..($parts.Count - 2)] | Where-Object { $_ }
    $k = $parts[-1]
    foreach ($m in $mods) { Put @($mod[$m]) }
    $c = Codes $k
    Put $c
    if ($c.Count -eq 2) { Put @(0xe0, ($c[1] -bor 0x80)) } else { Put @(($c[0] -bor 0x80)) }
    [array]::Reverse($mods) 2>$null
    foreach ($m in $mods) { Put @(($mod[$m] -bor 0x80)) }
}
function Click($x, $y, $btn, $n) {
    $mouse.PutMouseEventAbsolute($x, $y, 0, 0, 0); Start-Sleep -Milliseconds 80
    for ($i = 0; $i -lt $n; $i++) {
        $mouse.PutMouseEventAbsolute($x, $y, 0, 0, $btn); Start-Sleep -Milliseconds 60
        $mouse.PutMouseEventAbsolute($x, $y, 0, 0, 0);    Start-Sleep -Milliseconds 60
    }
}

try {
    foreach ($line in Get-Content C:\Users\Admin\vmctl_cmds.txt) {
        $p = $line.Trim().Split(' ', 2)
        switch ($p[0]) {
            'click'    { $a = $p[1].Split(' '); Click ([int]$a[0]) ([int]$a[1]) 1 1 }
            'dblclick' { $a = $p[1].Split(' '); Click ([int]$a[0]) ([int]$a[1]) 1 2 }
            'rclick'   { $a = $p[1].Split(' '); Click ([int]$a[0]) ([int]$a[1]) 2 1 }
            'move'     { $a = $p[1].Split(' '); $mouse.PutMouseEventAbsolute([int]$a[0], [int]$a[1], 0, 0, 0) }
            'drag'     { $a = $p[1].Split(' ')
                         $mouse.PutMouseEventAbsolute([int]$a[0], [int]$a[1], 0, 0, 0); Start-Sleep -Milliseconds 100
                         $mouse.PutMouseEventAbsolute([int]$a[0], [int]$a[1], 0, 0, 1); Start-Sleep -Milliseconds 150
                         for ($k = 1; $k -le 10; $k++) {
                             $x = [int]$a[0] + ([int]$a[2] - [int]$a[0]) * $k / 10
                             $y = [int]$a[1] + ([int]$a[3] - [int]$a[1]) * $k / 10
                             $mouse.PutMouseEventAbsolute([int]$x, [int]$y, 0, 0, 1); Start-Sleep -Milliseconds 60 }
                         $mouse.PutMouseEventAbsolute([int]$a[2], [int]$a[3], 0, 0, 0); Start-Sleep -Milliseconds 150 }
            'type'     { & $vbm controlvm winxp keyboardputstring $p[1] | Out-Null; Start-Sleep -Milliseconds 100 }
            'key'      { foreach ($k in $p[1].Split(',')) { Key $k.Trim() } }
            'sleep'    { Start-Sleep -Milliseconds ([int]$p[1]) }
            ''         { }
            default    { Write-Output "unknown command: $line" }
        }
    }
} finally {
    $session.UnlockMachine()
}
Remove-Item C:\Users\Admin\vm.png -ErrorAction SilentlyContinue
& $vbm controlvm winxp screenshotpng C:\Users\Admin\vm.png | Out-Null
Write-Output "done"
