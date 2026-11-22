# Tells what a Windows machine has of what the tests need: processor,
# virtualization, the graphics cards, and what there is for running VMs.
#
# It only reads, needs no administrator, changes nothing and installs nothing.
# What it writes is meant to be published: no computer name, no user name, no
# serial numbers, no network addresses, only the fields below.
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File hw\inventory.ps1
#
# writes hw-out\inventory.txt and shows it.  See hw\README.md.

$ErrorActionPreference = 'SilentlyContinue'
$version = 2
$out = Join-Path (Split-Path -Parent $PSScriptRoot) 'hw-out'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$file = Join-Path $out 'inventory.txt'
$lines = New-Object System.Collections.Generic.List[string]

function Kv([string]$Key, $Value) {
    $v = ([string]$Value -replace '\s+', ' ').Trim()
    if ($v.Length -gt 160) { $v = $v.Substring(0, 160) }
    $script:lines.Add(('{0}: {1}' -f $Key, $v))
}

Kv 'inventory' ("windows, version {0}" -f $version)
Kv 'date' ((Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm') + ' UTC')
$os = Get-CimInstance Win32_OperatingSystem
Kv 'os' ("{0}, {1}" -f $os.Caption, $os.Version)
Kv 'arch' $os.OSArchitecture
$cpu = @(Get-CimInstance Win32_Processor)[0]
Kv 'cpu' $cpu.Name
Kv 'cpu-threads' $cpu.NumberOfLogicalProcessors
Kv 'cpu-virtualization-in-firmware' $cpu.VirtualizationFirmwareEnabled
Kv 'cpu-second-level-address-translation' $cpu.SecondLevelAddressTranslationExtensions
$cs = Get-CimInstance Win32_ComputerSystem
Kv 'a-hypervisor-is-running' $cs.HypervisorPresent
Kv 'memory-gib' ([int]($cs.TotalPhysicalMemory / 1GB))
$drive = Get-PSDrive -Name ($PSScriptRoot.Substring(0, 1))
Kv 'free-disk-gib' ([int]($drive.Free / 1GB))

# The graphics cards, with the IDs of the device and without what tells one
# card of a kind from another.
$n = 0
foreach ($g in @(Get-CimInstance Win32_VideoController)) {
    $n++
    $ids = ''
    if ($g.PNPDeviceID -match '(VEN_[0-9A-Fa-f]{4}&DEV_[0-9A-Fa-f]{4})') { $ids = $Matches[1] }
    Kv ("graphics-{0}" -f $n) ("{0}, {1}, driver {2}, {3} MB as Windows tells it" -f $g.Name, $ids, $g.DriverVersion, [int]($g.AdapterRAM / 1MB))
}
Kv 'graphics-cards' $n

# Which kind of interrupt Windows runs the graphics cards and the sound
# controllers on the PCI bus with.  A small number is a line interrupt (INTx),
# numbers above four thousand million are messages (MSI or MSI-X).  It matters
# for a card which is to go to a VM: its sound function is such a controller.
$alloc = @(Get-CimInstance Win32_PnPAllocatedResource)
$n = 0
foreach ($d in @(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'PCI\*' })) {
    $hw = @($d.HardwareID) -join ' '
    if ($hw -notmatch 'CC_(03[0-9A-Fa-f]{2}|0403)') { continue }
    $cc = $Matches[1]
    $ids = ''
    if ($d.InstanceId -match '(VEN_[0-9A-Fa-f]{4}&DEV_[0-9A-Fa-f]{4})') { $ids = $Matches[1] }
    $irqs = @($alloc | Where-Object { $_.Dependent.DeviceID -eq $d.InstanceId -and $_.Antecedent.CimSystemProperties.ClassName -eq 'Win32_IRQResource' } |
              ForEach-Object { [uint32]$_.Antecedent.IRQNumber } | Sort-Object)
    $kind = 'no interrupt'
    if ($irqs.Count -gt 0) {
        if ($irqs[0] -lt 4096) { $kind = "a line interrupt (INTx), IRQ $($irqs[0])" } else { $kind = "$($irqs.Count) message interrupt(s) (MSI or MSI-X)" }
    }
    $svc = [string](Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName 'DEVPKEY_Device_Service').Data
    $n++
    Kv ("interrupt-{0}" -f $n) ("class {0}, {1}, {2}, driver {3}: {4}" -f $cc, $ids, $d.FriendlyName, $svc, $kind)
}
Kv 'graphics-and-sound-controllers' $n

# Whether the firmware starts with Secure Boot; asking takes an administrator.
$sb = 'not told without an administrator'
try { $sb = [string](Confirm-SecureBootUEFI -ErrorAction Stop) } catch { }
Kv 'secure-boot' $sb

# Whether the drive of Windows is encrypted with BitLocker, as the shell
# tells it to everybody: a change to the settings of the firmware can have
# such a drive ask for its recovery key at the next start.
$bl = 'not told'
try { $bl = [string](New-Object -ComObject Shell.Application).NameSpace($env:SystemDrive + '\').Self.ExtendedProperty('System.Volume.BitLockerProtection') } catch { }
Kv 'bitlocker-on-the-drive-of-windows' ("{0} (1 on, 2 off, 3 being encrypted, 5 suspended)" -f $bl)

# What there is for running VMs and for building.
$dg = Get-CimInstance -Namespace 'root\Microsoft\Windows\DeviceGuard' -ClassName Win32_DeviceGuard
if ($dg) { Kv 'virtualization-based-security' ("status {0}, services running {1}" -f $dg.VirtualizationBasedSecurityStatus, (@($dg.SecurityServicesRunning) -join ',')) }
$wsl = 'not installed'
if (Get-Command wsl.exe) {
    $null = & wsl.exe --status 2>&1
    $wsl = "wsl.exe is there, --status ended with $LASTEXITCODE"
}
Kv 'wsl' $wsl
$vbm = $null
foreach ($p in @((Get-Command VBoxManage.exe).Source, (Join-Path $env:ProgramFiles 'Oracle\VirtualBox\VBoxManage.exe'))) {
    if ($p -and (Test-Path -LiteralPath $p)) { $vbm = $p; break }
}
if ($vbm) { Kv 'virtualbox' (& $vbm --version 2>&1 | Select-Object -First 1) } else { Kv 'virtualbox' 'not installed' }
foreach ($t in 'git.exe', 'cl.exe', 'gcc.exe') {
    if (Get-Command $t) { Kv ("tool-{0}" -f $t) 'there' } else { Kv ("tool-{0}" -f $t) 'not found' }
}

# Nothing which names this machine or its user may be in there.
foreach ($word in @($env:USERNAME, $env:COMPUTERNAME)) {
    if ($word -and $word.Length -ge 3) {
        $re = '(?i)(^|[^A-Za-z0-9])' + [regex]::Escape($word) + '($|[^A-Za-z0-9])'
        if ($lines | Where-Object { $_ -match $re }) {
            Write-Output 'The result has the name of this machine or of its user in it and was not written.'
            exit 1
        }
    }
}

# Without a byte order mark and with the line ends git expects.
[System.IO.File]::WriteAllText($file, (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
$lines
''
"Written to $file ($($lines.Count) lines). Publish it with: sh hw/publish.sh (Git Bash) or copy its text."
exit 0
