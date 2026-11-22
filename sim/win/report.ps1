# Part of the simulation, see ../run.sh: runs in the Windows guest and tells
# on the serial port what Windows made of the PCI root bridge and of the
# hot-plug capable slot.  Every line starts with "WIN: ".  After the report
# it keeps saying that it is alive, and tells when a device comes to the bus
# behind a bridge or goes from it.
#
# It only reads.  Started by first.cmd once Windows is installed, and by a
# scheduled task at each later start.
#
#   report.ps1 [-Port COM1] [-AliveSeconds 7200] [-Console]
#
# With -Console the lines go to the console instead of the serial port, for
# trying the script on a Windows which is at hand.

param(
    [string]$Port = 'COM1',
    [int]$AliveSeconds = 7200,
    [switch]$Console
)

$ErrorActionPreference = 'Continue'

$serial = $null
if (-not $Console) {
    for ($try = 0; $try -lt 20 -and -not $serial; $try++) {
        try {
            $p = New-Object System.IO.Ports.SerialPort $Port, 115200, ([System.IO.Ports.Parity]::None), 8, ([System.IO.Ports.StopBits]::One)
            $p.WriteTimeout = 5000
            $p.NewLine = "`r`n"
            $p.Open()
            $serial = $p
        } catch {
            Start-Sleep -Seconds 3
        }
    }
}

function Say([string]$Text) {
    $line = 'WIN: ' + ($Text -replace '[\r\n\t]+', ' ')
    if ($line.Length -gt 300) { $line = $line.Substring(0, 300) }
    if ($script:serial) {
        try { $script:serial.WriteLine($line) } catch { }
    } else {
        [Console]::Out.WriteLine($line)
    }
}

function Prop([string]$InstanceId, [string]$KeyName) {
    try {
        $v = (Get-PnpDeviceProperty -InstanceId $InstanceId -KeyName $KeyName -ErrorAction Stop).Data
        if ($null -eq $v) { return '' }
        return [string]$v
    } catch {
        return ''
    }
}

# The properties of a device whose names match, one line each.
function Show-Props([string]$What, [string]$InstanceId, [string]$Pattern) {
    try {
        $props = @(Get-PnpDeviceProperty -InstanceId $InstanceId -ErrorAction Stop | Where-Object { $_.KeyName -match $Pattern })
    } catch {
        Say ("{0}-PROPS failed: {1}" -f $What, $_.Exception.Message)
        return
    }
    foreach ($pr in $props) {
        Say ("{0}-PROP {1}={2}" -f $What, $pr.KeyName, (@($pr.Data) -join ','))
    }
    Say ("{0}-PROPS n={1}" -f $What, $props.Count)
}

function Problem-Code($Device) {
    try { return [int]$Device.ConfigManagerErrorCode } catch { return -1 }
}

# The PCI devices which are there, as "bus.device.function id" lines.
function Pci-Devices {
    $list = @()
    $devs = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'PCI\*' })
    if ($devs.Count -eq 0) { return $list }
    # Bus number and address of all of them with one question each.
    $busOf = @{}
    $adrOf = @{}
    $ids = @($devs | ForEach-Object { $_.InstanceId })
    foreach ($pr in @(Get-PnpDeviceProperty -InstanceId $ids -KeyName 'DEVPKEY_Device_BusNumber', 'DEVPKEY_Device_Address' -ErrorAction SilentlyContinue)) {
        if ($null -eq $pr.Data) { continue }
        if ($pr.KeyName -eq 'DEVPKEY_Device_BusNumber') { $busOf[$pr.InstanceId] = [string]$pr.Data }
        else                                             { $adrOf[$pr.InstanceId] = [uint32]$pr.Data }
    }
    foreach ($d in $devs) {
        $bus = ''
        $dev = ''
        $fun = ''
        if ($busOf.ContainsKey($d.InstanceId)) { $bus = $busOf[$d.InstanceId] }
        if ($adrOf.ContainsKey($d.InstanceId)) {
            $dev = ($adrOf[$d.InstanceId] -shr 16) -band 0xffff
            $fun = $adrOf[$d.InstanceId] -band 0xffff
        }
        $list += New-Object PSObject -Property @{
            Where   = ("{0}.{1}.{2}" -f $bus, $dev, $fun)
            Bus     = $bus
            Device  = $d
        }
    }
    return $list
}


Say 'UP'
$os = Get-CimInstance Win32_OperatingSystem
Say ("OS={0} {1}" -f $os.Caption, $os.Version)
Say ("CPUS={0} MEM_MB={1} UPTIME_S={2}" -f [Environment]::ProcessorCount, [int]($os.TotalVisibleMemorySize / 1024), [int]((Get-Date) - $os.LastBootUpTime).TotalSeconds)

# The root bridge: PNP0A08 is PCI Express, PNP0A03 is PCI.  Its properties
# tell what the firmware handed over when Windows asked (_OSC).
$roots = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^ACPI\\PNP0A0[38]\\' })
foreach ($r in $roots) {
    Say ("ROOTBUS id={0} status={1} problem={2} service={3}" -f $r.InstanceId, $r.Status, (Problem-Code $r), (Prop $r.InstanceId 'DEVPKEY_Device_Service'))
    Show-Props 'ROOTBUS' $r.InstanceId '^DEVPKEY_PciRootBus_|^\{D817FC28'
}
Say ("ROOTBUSES n={0}" -f $roots.Count)

# All PCI devices, and more about the bridges.
$pci = @(Pci-Devices)
$ports = 0
foreach ($e in $pci) {
    $d = $e.Device
    $ids = @($d.HardwareID) -join ' '
    $cc = ''
    if ($ids -match 'CC_([0-9A-Fa-f]{4})') { $cc = $Matches[1] }
    Say ("DEV at={0} id={1} cc={2} class={3} status={4} problem={5} service={6} name={7}" -f $e.Where, $d.InstanceId, $cc, $d.Class, $d.Status, (Problem-Code $d), (Prop $d.InstanceId 'DEVPKEY_Device_Service'), $d.FriendlyName)
    if ($cc -eq '0604') {
        $ports++
        Say ("PORT at={0} id={1} status={2} problem={3} service={4}" -f $e.Where, $d.InstanceId, $d.Status, (Problem-Code $d), (Prop $d.InstanceId 'DEVPKEY_Device_Service'))
        Show-Props 'PORT' $d.InstanceId '^DEVPKEY_PciDevice_|^DEVPKEY_Device_(DriverInfPath|DriverVersion|LocationInfo|ProblemStatus|DevNodeStatus)$'
    }
}
Say ("DEVS n={0} ports={1}" -f $pci.Count, $ports)

# Devices of any kind which Windows has a problem with.
$bad = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { (Problem-Code $_) -ne 0 })
foreach ($d in $bad) {
    Say ("PROBLEM code={0} id={1} class={2} name={3}" -f (Problem-Code $d), $d.InstanceId, $d.Class, $d.FriendlyName)
}
Say ("PROBLEMS n={0}" -f $bad.Count)

# What the system log has from the parts which deal with the firmware tables
# and the PCI bus, and whether Windows crashed since it was installed.
$ev = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; Level = 1, 2, 3 } -MaxEvents 500 -ErrorAction SilentlyContinue |
        Where-Object { $_.ProviderName -match 'ACPI|^pci$|PCI|WHEA|Kernel-PnP|BugCheck' })
foreach ($e in ($ev | Select-Object -First 15)) {
    Say ("EVENT id={0} level={1} provider={2} text={3}" -f $e.Id, $e.Level, $e.ProviderName, $e.Message)
}
Say ("EVENTS n={0}" -f $ev.Count)
$crash = @(Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'Microsoft-Windows-WER-SystemErrorReporting' } -MaxEvents 20 -ErrorAction SilentlyContinue)
foreach ($e in ($crash | Select-Object -First 3)) {
    Say ("BUGCHECK id={0} text={1}" -f $e.Id, $e.Message)
}
Say ("BUGCHECKS n={0}" -f $crash.Count)

# The power button as Windows has it, and what Windows does when it is
# pressed (0 nothing, 1 sleep, 2 hibernate, 3 shut down, 4 display off).
foreach ($b in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match '^ACPI\\(FixedButton|PNP0C0C|PNP0C0E)' })) {
    Say ("BUTTON id={0} status={1} problem={2} name={3}" -f $b.InstanceId, $b.Status, (Problem-Code $b), $b.FriendlyName)
}
try {
    $q = (& powercfg.exe /qh SCHEME_CURRENT SUB_BUTTONS PBUTTONACTION 2>&1 | Out-String)
    $idx = [regex]::Matches($q, '0x[0-9a-fA-F]{8}')
    if ($idx.Count -ge 2) {
        Say ("BUTTON-ACTION ac={0} dc={1}" -f $idx[$idx.Count - 2].Value, $idx[$idx.Count - 1].Value)
    } else {
        Say 'BUTTON-ACTION unknown'
    }
} catch {
    Say ("BUTTON-ACTION failed: {0}" -f $_.Exception.Message)
}

# The TPM, where the VM has one: what Windows has made of it.  Asking for
# it has Windows send commands to the TPM.
# A TPM which Windows sees for the first time takes a moment until its
# driver has it, so where the firmware tables have one, this waits for it.
$tpmRe = '^ACPI\\(MSFT0101|PNP0C31)'
$tpmNodes = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match $tpmRe })
$tpm = @()
$tpmErr = ''
for ($try = 0; $try -lt 18; $try++) {
    try {
        $tpm = @(Get-CimInstance -Namespace 'root/cimv2/Security/MicrosoftTpm' -ClassName Win32_Tpm -ErrorAction Stop)
        $tpmErr = ''
    } catch {
        $tpmErr = $_.Exception.Message
    }
    if ($tpm.Count -gt 0 -or $tpmNodes.Count -eq 0) { break }
    Start-Sleep -Seconds 5
}
if ($tpm.Count -gt 0) {
    Say ("TPM present=1 spec={0} manufacturer={1} version={2} enabled={3} activated={4}" -f $tpm[0].SpecVersion, $tpm[0].ManufacturerIdTxt, $tpm[0].ManufacturerVersion, $tpm[0].IsEnabled_InitialValue, $tpm[0].IsActivated_InitialValue)
} else {
    Say ("TPM present=0 nodes={0} {1}" -f $tpmNodes.Count, $tpmErr)
}
foreach ($t in @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -match $tpmRe })) {
    Say ("TPMDEV id={0} status={1} problem={2} service={3}" -f $t.InstanceId, $t.Status, (Problem-Code $t), (Prop $t.InstanceId 'DEVPKEY_Device_Service'))
}
Say 'REPORT-END'

# Keeps running: says that it is alive, tells when a device behind a bridge
# comes or goes, reads from an NVMe drive which came, and shuts Windows down
# when the outside asks for it.
function Behind-Bridges {
    $now = @{}
    foreach ($e in @(Pci-Devices)) {
        if ($e.Bus -ne '' -and $e.Bus -ne '0') {
            $now[$e.Where] = ("{0} status={1} problem={2} service={3}" -f $e.Device.InstanceId, $e.Device.Status, (Problem-Code $e.Device), (Prop $e.Device.InstanceId 'DEVPKEY_Device_Service'))
        }
    }
    return $now
}

# The MD5 checksum of the first bytes of a file or of a drive, read past any
# cache: a megabyte at a time, which a drive takes as it is.
function Md5-OfStart([string]$Path, [int]$Bytes) {
    try {
        $fs = New-Object System.IO.FileStream($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite, 1048576)
        try {
            $md5 = [System.Security.Cryptography.MD5]::Create()
            $buf = New-Object byte[] 1048576
            $left = $Bytes
            while ($left -gt 0) {
                $n = $fs.Read($buf, 0, [Math]::Min($buf.Length, $left))
                if ($n -le 0) { break }
                [void]$md5.TransformBlock($buf, 0, $n, $null, 0)
                $left -= $n
            }
            [void]$md5.TransformFinalBlock($buf, 0, 0)
            $sum = ($md5.Hash | ForEach-Object { $_.ToString('x2') }) -join ''
            if ($left -gt 0) { return ("{0} short-by-{1}" -f $sum, $left) }
            return $sum
        } finally {
            $fs.Close()
        }
    } catch {
        return ("failed: {0}" -f $_.Exception.Message)
    }
}

# Reads from the drive of an NVMe controller once Windows has made a disk of
# it.  The controller is given by where it is ("bus.device.function") and by
# its instance ID: a device with two functions has two controllers and two
# disks, and the disk of a controller is the one whose parent it is.  The
# lines about a function other than the first have a 2 after their first
# word (READ2, DRIVE2, NODRIVE2).
function Read-Drive([int]$Round, [string]$Where, [string]$Controller) {
    $tag = ''
    if ($Where -notmatch '\.0$') { $tag = '2' }
    $disk = $null
    $seenParents = ''
    for ($t = 0; $t -lt 20 -and -not $disk; $t++) {
        $all = @(Get-CimInstance Win32_DiskDrive -ErrorAction SilentlyContinue |
                 Where-Object { $_.PNPDeviceID -match 'VEN_NVME' -or $_.Model -match 'NVMe' })
        $parents = @()
        foreach ($c in $all) {
            $parent = Prop $c.PNPDeviceID 'DEVPKEY_Device_Parent'
            if ($parent -ne '') { $parents += $parent }
            if ($parent -ne '' -and $parent -eq $Controller -and -not $disk) { $disk = $c }
        }
        $seenParents = $parents -join ','
        # Nothing says whose disk the one disk is, or it names somebody else:
        # the first function takes it, as it did when there was nothing to
        # choose from, and the line below tells.
        if (-not $disk -and $all.Count -eq 1 -and $tag -eq '' -and $t -ge 3) { $disk = $all[0] }
        if (-not $disk) { Start-Sleep -Seconds 3 }
    }
    if (-not $disk) {
        Say ("NODRIVE{0} {1} controller={2} parents={3}" -f $tag, $Round, $Controller, $seenParents)
        return
    }
    if ((Prop $disk.PNPDeviceID 'DEVPKEY_Device_Parent') -ne $Controller) {
        Say ("DRIVE-NOTE {0} the disk does not name the controller as its parent: controller={1} parents={2}" -f $Round, $Controller, $seenParents)
    }
    Say ("DRIVE{0} {1} {2} model={3} size={4}" -f $tag, $Round, $disk.DeviceID, $disk.Model, $disk.Size)
    Say ("READ{0} {1} md5={2}" -f $tag, $Round, (Md5-OfStart $disk.DeviceID 8388608))
}

# What the outside asks for: a DVD with the directory simcmd in the drive.
function Host-Command {
    try {
        foreach ($dr in [System.IO.DriveInfo]::GetDrives()) {
            if ($dr.DriveType -eq [System.IO.DriveType]::CDRom -and $dr.IsReady) {
                if (Test-Path -LiteralPath (Join-Path $dr.RootDirectory.FullName 'simcmd\shutdown')) { return 'shutdown' }
                if (Test-Path -LiteralPath (Join-Path $dr.RootDirectory.FullName 'simcmd\intx')) { return 'intx' }
            }
        }
    } catch { }
    return ''
}

# The interrupts Windows has given a device, as numbers with commas between: a
# line interrupt has a small number, the messages of MSI and MSI-X have very
# large ones.
function Irq-Numbers([string]$InstanceId) {
    try {
        $esc = $InstanceId -replace '\\', '\\'
        $res = @(Get-CimInstance -Query ("ASSOCIATORS OF {Win32_PnPEntity.DeviceID='" + $esc + "'} WHERE ResultClass = Win32_IRQResource") -ErrorAction Stop)
        if ($res.Count -eq 0) { return 'none' }
        return (($res | ForEach-Object { $_.IRQNumber } | Sort-Object | Select-Object -First 4) -join ',')
    } catch {
        return ('failed-' + ($_.Exception.Message -replace '\s+', '_'))
    }
}

# Has Windows run the NVMe controllers behind a bridge with a line interrupt
# (INTx) instead of MSI-X: the setting for that is written for each, the device
# is started anew, and it tells of the interrupt it has then and reads from the
# drive again.
function Use-LineInterrupt([int]$Round) {
    $now = Behind-Bridges
    foreach ($k in ($now.Keys | Sort-Object)) {
        if ($now[$k] -notmatch 'service=stornvme') { continue }
        $id = ($now[$k] -split ' status=')[0]
        Say ("INTX-BEFORE {0} at={1} irqs={2}" -f $Round, $k, (Irq-Numbers $id))
        try {
            $key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\' + $id + '\Device Parameters\Interrupt Management\MessageSignaledInterruptProperties'
            if (-not (Test-Path -LiteralPath $key)) { New-Item -Path $key -Force -ErrorAction Stop | Out-Null }
            Set-ItemProperty -LiteralPath $key -Name 'MSISupported' -Value 0 -Type DWord -ErrorAction Stop
        } catch {
            Say ("INTX-FAILED {0} at={1} setting: {2}" -f $Round, $k, $_.Exception.Message)
            continue
        }
        $out = (& pnputil.exe /restart-device $id 2>&1 | Out-String) -replace '\s+', ' '
        Say ("INTX-RESTART {0} at={1} {2}" -f $Round, $k, $out.Trim())
        $status = 'gone'
        for ($t = 0; $t -lt 45; $t++) {
            Start-Sleep -Seconds 2
            $d = Get-PnpDevice -InstanceId $id -ErrorAction SilentlyContinue
            if ($d) { $status = $d.Status }
            if ($status -eq 'OK') { break }
        }
        Say ("INTX {0} at={1} status={2} irqs={3}" -f $Round, $k, $status, (Irq-Numbers $id))
        if ($status -eq 'OK') { Read-Drive $Round $k $id }
    }
    Say ("INTX-DONE {0}" -f $Round)
}

$intxDone = $false
$seen = Behind-Bridges
foreach ($k in $seen.Keys) {
    Say ("PRESENT 0 at={0} id={1}" -f $k, $seen[$k])
    if ($seen[$k] -match 'status=OK problem=0 service=stornvme') { Read-Drive 0 $k (($seen[$k] -split ' status=')[0]) }
}
$rounds = [int]($AliveSeconds / 5)
for ($i = 1; $i -le $rounds; $i++) {
    Say ("ALIVE {0}" -f $i)
    Start-Sleep -Seconds 5
    $cmd = Host-Command
    if ($cmd -eq 'intx' -and -not $intxDone) {
        $intxDone = $true
        Use-LineInterrupt $i
        $seen = Behind-Bridges
        continue
    }
    if ($cmd -eq 'shutdown') {
        Say 'SHUTDOWN'
        & shutdown.exe /s /t 0 /f
        break
    }
    $now = Behind-Bridges
    foreach ($k in $now.Keys) {
        if (-not $seen.ContainsKey($k) -or $seen[$k] -ne $now[$k]) {
            Say ("PRESENT {0} at={1} id={2}" -f $i, $k, $now[$k])
            if ($now[$k] -match 'status=OK problem=0 service=stornvme') { Read-Drive $i $k (($now[$k] -split ' status=')[0]) }
        }
    }
    foreach ($k in $seen.Keys) {
        if (-not $now.ContainsKey($k)) { Say ("GONE {0} at={1}" -f $i, $k) }
    }
    $seen = $now
}
if ($serial) { $serial.Close() }
