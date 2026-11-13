#requires -version 5.1
<#
.SYNOPSIS
Extends C: into the unallocated space after it (the app grows the disk image to 64 GB; Windows does
not use the new space until the partition is extended). Run elevated. Safe to run again: it does
nothing when C: already fills the disk.
Exit codes: 0 extended or nothing to gain, 1 failed, 3 another partition sits in the way.
.PARAMETER WhatIf
Only report what would happen.
#>
[CmdletBinding()]
param([switch]$WhatIf)

$ErrorActionPreference = 'Stop'
$dir = 'C:\ProgramData\WindowsiOS'
$log = Join-Path $dir 'extend-c.txt'
$min = 1MB  # the gap a disk leaves after C: for its backup GPT and alignment is below this

function Say($m) {
    Write-Host "  $m"
    try {
        if (-not $WhatIf) {
            if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir -Force | Out-Null }
            Add-Content -Path $log -Value ('{0:s} {1}' -f (Get-Date), $m)
        }
    } catch { }
}
function Gb($b) { '{0:N1} GB' -f ($b / 1GB) }

try {
    $id = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not $id.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        Write-Host '  Run this elevated (as administrator).'
        exit 1
    }
    # The cache can hold the disk's size from before the image grew.
    Update-HostStorageCache -ErrorAction SilentlyContinue
    $c = Get-Partition -DriveLetter C
    $disk = Get-Disk -Number $c.DiskNumber
    $sup = Get-PartitionSupportedSize -DriveLetter C
    $gain = [int64]$sup.SizeMax - [int64]$c.Size
    Say ('Disk {0}, C: {1}, largest C: possible {2}' -f (Gb $disk.Size), (Gb $c.Size), (Gb $sup.SizeMax))

    if ($gain -lt $min) {
        # Space that exists but is not next to C: (a recovery partition after it) cannot be reached.
        $after = @(Get-Partition -DiskNumber $c.DiskNumber | Where-Object { $_.Offset -gt $c.Offset } | Sort-Object Offset)
        if ($after.Count -gt 0) {
            $end = $after[-1].Offset + $after[-1].Size
            $free = [int64]$disk.Size - $end
            if ($free -ge $min) {
                $kinds = ($after | ForEach-Object { 'partition {0} ({1}, {2})' -f $_.PartitionNumber, $_.Type, (Gb $_.Size) }) -join '; '
                Say "Not extended: $kinds follows C: and blocks the free $(Gb $free) at the end of the disk."
                Say 'It is not touched here. For a recovery partition: reagentc /disable, delete that partition (diskpart), run this again,'
                Say 'then create a new recovery partition at the end (type DE94BBA4-06D1-4D40-A16A-BFD50179D6AC) and reagentc /enable.'
                exit 3
            }
        }
        if ($disk.Size -le $c.Offset + $c.Size + $min) {
            Say 'C: already fills the disk. If the disk image was just made bigger and Windows still shows the old size, use Restart once (not Shut down).'
        } else {
            Say 'C: already uses all the space that follows it. Nothing to do.'
        }
        exit 0
    }

    if ($WhatIf) {
        Say ('Would extend C: from {0} to {1}.' -f (Gb $c.Size), (Gb $sup.SizeMax))
        exit 0
    }
    Resize-Partition -DriveLetter C -Size $sup.SizeMax
    $n = Get-Partition -DriveLetter C
    if ($n.Size -le $c.Size) { throw 'C: did not grow' }
    Say ('Extended C: from {0} to {1}.' -f (Gb $c.Size), (Gb $n.Size))
    exit 0
} catch {
    Say ('FAILED: ' + $_.Exception.Message)
    exit 1
}
