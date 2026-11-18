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
Say 'REPORT-END'

# Keeps running: says that it is alive, and tells when a device behind a
# bridge comes or goes.
function Behind-Bridges {
    $now = @{}
    foreach ($e in @(Pci-Devices)) {
        if ($e.Bus -ne '' -and $e.Bus -ne '0') {
            $now[$e.Where] = ("{0} status={1} problem={2} service={3}" -f $e.Device.InstanceId, $e.Device.Status, (Problem-Code $e.Device), (Prop $e.Device.InstanceId 'DEVPKEY_Device_Service'))
        }
    }
    return $now
}

$seen = Behind-Bridges
foreach ($k in $seen.Keys) { Say ("PRESENT 0 at={0} id={1}" -f $k, $seen[$k]) }
$rounds = [int]($AliveSeconds / 5)
for ($i = 1; $i -le $rounds; $i++) {
    Say ("ALIVE {0}" -f $i)
    Start-Sleep -Seconds 5
    $now = Behind-Bridges
    foreach ($k in $now.Keys) {
        if (-not $seen.ContainsKey($k) -or $seen[$k] -ne $now[$k]) { Say ("PRESENT {0} at={1} id={2}" -f $i, $k, $now[$k]) }
    }
    foreach ($k in $seen.Keys) {
        if (-not $now.ContainsKey($k)) { Say ("GONE {0} at={1}" -f $i, $k) }
    }
    $seen = $now
}
if ($serial) { $serial.Close() }
