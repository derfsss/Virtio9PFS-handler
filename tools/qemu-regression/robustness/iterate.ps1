# iterate.ps1 — single fix-iteration cycle.  v2 strategy.
#
# Why this design:
#   - QEMU AmigaOne with our handler in L: requires a hot-reload of the
#     binary between fix-iterations.  C:Reboot from inside the guest
#     does not work; QMP `quit` between two QEMU runs torn-writes the
#     SmartFilesystem metadata on hd0.qcow2 and SFS prompts for
#     "blockid error" recovery on the next boot.
#   - So: restore hd0.qcow2 from the clean master before each iteration,
#     do everything inside ONE QEMU launch, hot-swap the handler with
#     `Dismount SHARED:` + `Mount SHARED:`, then QMP-quit.  Corruption
#     to the working qcow2 is fine because the next iteration restores
#     it from master anyway.
#
# Steps:
#   1. Stop any leftover QEMU.
#   2. Restore E:\...\backup-base_a1_copy\hd0.qcow2 from
#      E:\...\claude-backup-base_a1\hd0.qcow2 (byte-identical clean copy).
#   3. Stage the fresh handler binary into S:\temp\.
#   4. Launch QEMU.   Wait for SerialShell on 4321.
#   5. From the guest:
#        a. Copy SHARED:Virtio9PFS-handler-new -> L:Virtio9PFS-handler
#        b. Update                     (flush DOS buffers)
#        c. Dismount SHARED:           (handler exits)
#        d. Mount SHARED:              (handler reloads from L:)
#   6. Run the robustness runner.
#   7. QMP `quit` (or hard kill on failure).

[CmdletBinding()]
param(
    [string]$InstallSrc = "C:\msys64\home\rich_\Projects\Virtio9PFS-handler\build\Virtio9PFS-handler.debug",
    [string]$Tiers = "",
    [int]   $BootTimeoutSec = 240,
    [int]   $TestTimeoutSec = 1800,
    [switch]$KeepRunning,
    [switch]$NoInstall                 # use whatever's already in L:
)

$ErrorActionPreference = "Stop"

$RepoRoot       = "C:\msys64\home\rich_\Projects\Virtio9PFS-handler"
$ShareRoot      = "S:\temp"
$StagedBin      = "$ShareRoot\Virtio9PFS-handler-new"
# Pidfile + port range chosen so this script never collides with any
# other QEMU instance the user may have running.  We *only* kill the
# PID listed in this pidfile — never a broad qemu-system-ppc sweep.
$PidFile        = "C:\Users\rich_\.kyvos\kyvos.iterate-base_a1.pid"
$SerialPort     = 4322          # host port -> guest TCP 4321 (SerialShell)
$QmpPort        = 14322
$QcowImage      = "E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\hd0.qcow2"
$LogDir         = "$RepoRoot\tools\qemu-regression\robustness\.runs"
$null = New-Item -ItemType Directory -Force -Path $LogDir
$RunStamp       = Get-Date -Format "yyyyMMdd-HHmmss"
$SerialLog      = "$LogDir\serial-$RunStamp.log"

$QemuExe = "E:\Emulators\QEMU\QEMU_Install\qemu-system-ppc.exe"

function Build-QemuCmdLine {
    param([string]$SerialLog)
    return @(
        '-M amigaone',
        '-kernel "C:\Users\rich_\.kyvos\bboot"',
        '-device loader,addr=0x600000,file="E:\Emulators\QEMU\QEMU_Machines\backup-base_a1_copy\kickstart.zip"',
        '-rtc base=localtime',
        '-accel tcg',
        '-vga none',
        "-chardev file,id=charserial0,path=`"$SerialLog`"",
        '-serial chardev:charserial0',
        '-device rtl8139,addr=0x0a,netdev=nic',
        # host SerialPort -> guest 4321 (SerialShell) — see $SerialPort above
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
        # cache=writethrough so device writes are sync; we still rely on
        # the 'restore from master' step to absorb any SFS torn writes.
        "-drive file=`"$QcowImage`",if=none,id=vd0,format=qcow2,cache=writethrough",
        '-device scsi-hd,drive=vd0,bus=scsi0.0,channel=0,scsi-id=0,lun=0',
        '-drive file="E:\Emulators\QEMU\QEMU_Machines\work.qcow2",if=none,id=vd1,format=qcow2,cache=writethrough',
        '-device scsi-hd,drive=vd1,bus=scsi0.0,channel=0,scsi-id=1,lun=0',
        "-fsdev local,security_model=mapped-xattr,id=fsdev0,path=`"$ShareRoot`"",
        '-device virtio-9p-pci,id=fs0,fsdev=fsdev0,mount_tag=SHARED',
        "-qmp tcp:127.0.0.1:${QmpPort},server=on,wait=off"
    ) -join ' '
}

function Wait-Port {
    param([int]$Port,[int]$TimeoutSec,[string]$Label="port")
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        try {
            $sock = New-Object System.Net.Sockets.TcpClient
            $iar = $sock.BeginConnect("127.0.0.1",$Port,$null,$null)
            if ($iar.AsyncWaitHandle.WaitOne(2000) -and $sock.Connected) {
                $sock.Close(); return $true
            }
            $sock.Close()
        } catch {}
        Start-Sleep -Seconds 2
    }
    Write-Warning "[iterate] $Label did not open within ${TimeoutSec}s"
    return $false
}

function Get-OurQemuPid {
    # Returns the PID of the QEMU we own (per pidfile), or $null.
    if (-not (Test-Path $PidFile)) { return $null }
    try {
        $line = (Get-Content $PidFile -ErrorAction Stop -Raw).Trim()
        if (-not $line) { return $null }
        $myPid = [int]$line
        # Confirm a qemu-system-ppc with that PID actually exists.
        $proc = Get-Process -Id $myPid -ErrorAction SilentlyContinue
        if ($proc -and $proc.ProcessName -eq "qemu-system-ppc") { return $myPid }
    } catch {}
    return $null
}

function Stop-Qemu-Hard {
    # Only kill the PID inside our pidfile — never a broad sweep of all
    # qemu-system-ppc processes.  The user runs other QEMUs in parallel.
    $myPid = Get-OurQemuPid
    if ($myPid) {
        Write-Host "[iterate]   killing OUR qemu pid $myPid (per pidfile)"
        Stop-Process -Id $myPid -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $PidFile) { Remove-Item $PidFile -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Seconds 2
}

function Stop-Qemu-QMP {
    try {
        $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1",$QmpPort)
        $stream = $client.GetStream()
        $reader = New-Object System.IO.StreamReader($stream)
        $writer = New-Object System.IO.StreamWriter($stream)
        $writer.NewLine = "`r`n"; $writer.AutoFlush = $true
        $null = $reader.ReadLine()
        $writer.WriteLine('{"execute":"qmp_capabilities"}'); $null = $reader.ReadLine()
        $writer.WriteLine('{"execute":"quit"}')
        try { $null = $reader.ReadLine() } catch {}
        $client.Close()
        Write-Host "[iterate]   QMP quit sent"
    } catch {
        Write-Warning "[iterate]   QMP quit failed: $_"
    }
    # Wait for OUR QEMU to exit (don't poll the global qemu-system-ppc list).
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-Date) -lt $deadline -and (Get-OurQemuPid)) {
        Start-Sleep -Seconds 1
    }
    Stop-Qemu-Hard  # belt-and-braces (still pidfile-scoped)
}

# ===== Main =====
# Per user policy: do NOT touch any qcow2 images other than the working
# one configured in the script.  If the working qcow2 becomes corrupt,
# ask the user where to restore from rather than guessing.
Stop-Qemu-Hard

# Stage handler
if (-not $NoInstall) {
    if (-not (Test-Path $InstallSrc)) {
        Write-Error "[iterate] handler binary not found: $InstallSrc"
        exit 2
    }
    Copy-Item -Force $InstallSrc $StagedBin
    Write-Host "[iterate] staged $InstallSrc -> $StagedBin"
}

# 3. Launch QEMU
Write-Host "[iterate] launching QEMU (serial -> $SerialLog)"
$cmdline = Build-QemuCmdLine -SerialLog $SerialLog
$proc = Start-Process -FilePath $QemuExe -ArgumentList $cmdline -PassThru -WindowStyle Minimized
Write-Host "[iterate] qemu pid=$($proc.Id)"

if (-not (Wait-Port -Port $SerialPort -TimeoutSec $BootTimeoutSec -Label "SerialShell port")) {
    Stop-Qemu-Hard
    exit 2
}

# Wait for the guest SerialShell daemon to actually answer.  TCP 4321 is
# bound by QEMU's hostfwd from the moment QEMU starts, so a TCP-level
# connect returns long before AmigaOS finishes booting.  Probe with a
# real SerialClient handshake, retrying until it succeeds.
Write-Host "[iterate] probing SerialShell handshake (boot may take ~60s)..."
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
            print('SerialShell up after %d attempts' % attempt)
            sys.exit(0)
    except Exception as e:
        pass
    time.sleep(3)
print('SerialShell never came up')
sys.exit(2)
"@
$probeFile = "$LogDir\boot-probe-$RunStamp.py"
$probe | Out-File -FilePath $probeFile -Encoding UTF8
& python $probeFile
if ($LASTEXITCODE -ne 0) {
    Write-Error "[iterate] SerialShell never responded; aborting"
    Stop-Qemu-Hard
    exit 2
}

# 4. Install + hot-swap (Dismount/Mount) + run tests, all in one Python pass
$env:PYTHONPATH    = "$RepoRoot\tools\qemu-regression"
$env:V9P_SHARE_HOST= $ShareRoot
$env:V9P_SERIAL_LOG= $SerialLog
$env:V9P_QMP       = "tcp:127.0.0.1:${QmpPort}"

$pyDriver = @"
import sys, time
sys.path.insert(0, r'$RepoRoot\tools\qemu-regression')
from robustness import common as cm
from robustness import runner as rn

# Point common.SerialClient at our forwarded port for the rest of the run.
cm.DEFAULT_PORT = $SerialPort

NO_INSTALL = '$($NoInstall.IsPresent)' == 'True'
TIERS      = r'$Tiers'

if not NO_INSTALL:
    print('[iterate] installing handler...', flush=True)
    c = cm.SerialClient('127.0.0.1', $SerialPort)
    c.connect()
    out = c.send_command('C:Copy SHARED:Virtio9PFS-handler-new TO L:Virtio9PFS-handler', timeout=30)
    print('  copy:', out.strip()[:200], flush=True)
    out = c.send_command('C:List L:Virtio9PFS-handler', timeout=10)
    print('  size:', out.strip().splitlines()[-1] if out.strip() else '(empty)', flush=True)
    out = c.send_command('Update', timeout=30)
    print('  Update:', repr(out.strip())[:80], flush=True)
    print('[iterate] hot-swap: Dismount SHARED:', flush=True)
    out = c.send_command('C:Dismount SHARED:', timeout=30)
    print('  Dismount:', repr(out.strip())[:200], flush=True)
    time.sleep(3)
    # Mount asynchronously via Run so the shell isn't blocked while the
    # new handler initialises (PCI re-init + 9P version + Attach can
    # easily take longer than the 30 s SerialShell command timeout).
    print('[iterate] hot-swap: Run Mount SHARED:', flush=True)
    out = c.send_command('C:Run >NIL: <NIL: C:Mount SHARED:', timeout=10)
    print('  Run Mount:', repr(out.strip())[:200], flush=True)
    # Poll for the new handler coming up
    mounted = False
    for attempt in range(30):
        time.sleep(2)
        try:
            out = c.send_command('C:Info SHARED:', timeout=10)
            if 'SHARED:' in out and ('Mounted' in out or '9PFP' in out):
                mounted = True
                print('  SHARED: mounted after %d polls' % (attempt + 1), flush=True)
                print(' ', out.strip().splitlines()[-1] if out.strip() else '', flush=True)
                break
        except Exception as e:
            print('  Info SHARED: poll exc:', e, flush=True)
    if not mounted:
        print('[iterate] FAIL: SHARED: did not come back after Mount', flush=True)
    c.close()
    if not mounted:
        sys.exit(2)

print('[iterate] runner...', flush=True)
argv = ['--skip-base', '--port', '$SerialPort',
        '--p9-share', r'$ShareRoot',
        '--qmp', r'tcp:127.0.0.1:${QmpPort}',
        '--serial-log', r'$SerialLog']
if TIERS:
    argv += ['--tiers', TIERS]
rc = rn.main(argv)
print('[iterate] runner rc=', rc, flush=True)
sys.exit(rc)
"@

$pyFile = "$LogDir\drive-$RunStamp.py"
$pyDriver | Out-File -FilePath $pyFile -Encoding UTF8

$rc = 0
try {
    & python $pyFile
    $rc = $LASTEXITCODE
} finally {
    if (-not $KeepRunning) {
        Write-Host "[iterate] QMP quit..."
        Stop-Qemu-QMP
    } else {
        Write-Host "[iterate] -KeepRunning: leaving QEMU up (pid $($proc.Id))"
    }
}

Write-Host "[iterate] DONE rc=$rc  (logs in $LogDir)"
exit $rc
