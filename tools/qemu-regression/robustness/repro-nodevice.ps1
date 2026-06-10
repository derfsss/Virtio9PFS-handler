# repro-nodevice.ps1 — boot the iterate.ps1 machine WITHOUT the virtio-9p
# device to exercise the handler's graceful mount-decline path (no device
# present).  Pass -WithDevice to include the device — useful for hot-swapping
# a new handler binary into L: between no-device runs.
# Same qcow2 images, pidfile, and ports as iterate.ps1 (user policy).

[CmdletBinding()]
param(
    [int]   $BootTimeoutSec = 240,
    [switch]$KeepRunning,
    [switch]$WithDevice            # include the virtio-9p device (normal config)
)

$ErrorActionPreference = "Stop"

$RepoRoot   = "C:\msys64\home\rich_\Projects\Virtio9PFS-handler"
$PidFile    = "C:\Users\rich_\.kyvos\kyvos.iterate-base_a1.pid"
$SerialPort = 4322
$QmpPort    = 14322
$QcowImage  = "E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\hd0.qcow2"
$LogDir     = "$RepoRoot\tools\qemu-regression\robustness\.runs"
$null = New-Item -ItemType Directory -Force -Path $LogDir
$RunStamp   = Get-Date -Format "yyyyMMdd-HHmmss"
$SerialLog  = "$LogDir\serial-nodev-$RunStamp.log"

$QemuExe = "E:\Emulators\QEMU\QEMU_Install\qemu-system-ppc.exe"

# Identical to iterate.ps1 Build-QemuCmdLine EXCEPT: no -fsdev / no
# -device virtio-9p-pci.
$cmdline = @(
    '-M amigaone',
    '-kernel "C:\Users\rich_\.kyvos\bboot"',
    '-device loader,addr=0x600000,file="E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\kickstart.zip"',
    '-rtc base=localtime',
    '-accel tcg',
    '-vga none',
    "-chardev file,id=charserial0,path=`"$SerialLog`"",
    '-serial chardev:charserial0',
    '-device rtl8139,addr=0x0a,netdev=nic',
    "-netdev user,id=nic,hostname=backup-base_a1_copy,hostfwd=tcp::${SerialPort}-:4321",
    '-append "serial debuglevel=1"',
    '-display sdl,gl=off,show-cursor=off,full-screen=off',
    '-device es1370,addr=0x09',
    '-name backup-base_a1_copy',
    '-m 2048M',
    '-drive if=none,id=cd',
    '-device ide-cd,unit=1,drive=cd,bus=ide.1',
    '-device sm501',
    "-pidfile `"$PidFile`"",
    '-device virtio-scsi-pci,id=scsi0',
    "-drive file=`"$QcowImage`",if=none,id=vd0,format=qcow2,cache=writethrough",
    '-device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0',
    '-drive file="E:\Emulators\QEMU\QEMU_Machines\work.qcow2",if=none,id=vd1,format=qcow2,cache=writethrough',
    '-device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=0',
    "-qmp tcp:127.0.0.1:${QmpPort},server=on,wait=off"
)
if ($WithDevice) {
    $cmdline += @(
        "-fsdev local,security_model=mapped-xattr,id=fsdev0,path=`"S:\temp`"",
        '-device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=SHARED'
    )
}
$cmdline = $cmdline -join ' '

function Get-OurQemuPid {
    if (-not (Test-Path $PidFile)) { return $null }
    try {
        $line = (Get-Content $PidFile -ErrorAction Stop -Raw).Trim()
        if (-not $line) { return $null }
        $myPid = [int]$line
        $proc = Get-Process -Id $myPid -ErrorAction SilentlyContinue
        if ($proc -and $proc.ProcessName -eq "qemu-system-ppc") { return $myPid }
    } catch {}
    return $null
}

function Stop-Qemu-Hard {
    $myPid = Get-OurQemuPid
    if ($myPid) {
        Write-Host "[repro]   killing OUR qemu pid $myPid (per pidfile)"
        Stop-Process -Id $myPid -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $PidFile) { Remove-Item $PidFile -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}

Stop-Qemu-Hard

$devDesc = if ($WithDevice) { "WITH" } else { "WITHOUT" }
Write-Host "[repro] launching QEMU $devDesc virtio-9p device (serial -> $SerialLog)"
$proc = Start-Process -FilePath $QemuExe -ArgumentList $cmdline -PassThru -WindowStyle Minimized
Write-Host "[repro] qemu pid=$($proc.Id)"

# Probe SerialShell with a real handshake (TCP port is open from t=0).
$env:PYTHONPATH = "$RepoRoot\tools\qemu-regression"
$probe = @"
import sys, time
sys.path.insert(0, r'$RepoRoot\tools\qemu-regression\robustness')
from common import SerialClient
deadline = time.time() + $BootTimeoutSec
attempt = 0
while time.time() < deadline:
    attempt += 1
    try:
        c = SerialClient('127.0.0.1', $SerialPort)
        c.connect(timeout=10)
        out = c.send_command('Echo READY', timeout=10)
        c.close()
        if 'READY' in out:
            print('SerialShell up after %d attempts (%.0fs)' % (attempt, time.time() - (deadline - $BootTimeoutSec)))
            sys.exit(0)
    except Exception:
        pass
    time.sleep(3)
print('SerialShell never came up within $BootTimeoutSec s')
sys.exit(2)
"@
$probeFile = "$LogDir\nodev-probe-$RunStamp.py"
$probe | Out-File -FilePath $probeFile -Encoding UTF8
& python $probeFile
$bootRc = $LASTEXITCODE

Write-Host "[repro] boot probe rc=$bootRc"
Write-Host "[repro] serial log: $SerialLog"
if ($KeepRunning) {
    # Keep this script (and thus the background task tree) alive while
    # QEMU runs -- task-completion cleanup would otherwise kill QEMU.
    Write-Host "[repro] waiting on QEMU (pid $($proc.Id)) -- QMP-quit or kill to end"
    Wait-Process -Id $proc.Id -ErrorAction SilentlyContinue
    Write-Host "[repro] QEMU exited"
}
exit $bootRc
