# Re-pair the Clawdmeter with Windows after a reflash left a stale bond
# (Windows can't heal it by itself; see clawdmeter-ble's lib.rs). Unpairs every
# paired "Clawdmeter", then pairs the one advertising. Windows PowerShell 5.1.
#   powershell -ExecutionPolicy Bypass -File tools\win_ble_repair.ps1
param([string]$Name = "Clawdmeter2", [int]$ScanSeconds = 15)

Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    $t.Result
}
[Windows.Devices.Enumeration.DeviceInformation, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
[Windows.Devices.Bluetooth.BluetoothLEDevice, Windows.Devices.Bluetooth, ContentType = WindowsRuntime] | Out-Null
$DI = [Windows.Devices.Enumeration.DeviceInformation]
$LE = [Windows.Devices.Bluetooth.BluetoothLEDevice]

# 1. Unpair every paired device with that name.
$paired = Await ($DI::FindAllAsync($LE::GetDeviceSelectorFromPairingState($true))) ([Windows.Devices.Enumeration.DeviceInformationCollection])
foreach ($d in $paired) {
    if ($d.Name -ne $Name) { continue }
    $r = Await ($d.Pairing.UnpairAsync()) ([Windows.Devices.Enumeration.DeviceUnpairingResult])
    Write-Output "unpaired $($d.Id): $($r.Status)"
}

# 2. Find it advertising (unpaired now) and pair.
$deadline = (Get-Date).AddSeconds($ScanSeconds)
while ((Get-Date) -lt $deadline) {
    $found = Await ($DI::FindAllAsync($LE::GetDeviceSelectorFromPairingState($false))) ([Windows.Devices.Enumeration.DeviceInformationCollection])
    $d = $found | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if ($d) {
        [Windows.Devices.Enumeration.DevicePairingProtectionLevel, Windows.Devices.Enumeration, ContentType = WindowsRuntime] | Out-Null
        $r = Await ($d.Pairing.PairAsync([Windows.Devices.Enumeration.DevicePairingProtectionLevel]::Encryption)) ([Windows.Devices.Enumeration.DevicePairingResult])
        Write-Output "paired $($d.Id): $($r.Status)"
        exit ([int]($r.Status -ne 'Paired' -and $r.Status -ne 'AlreadyPaired'))
    }
    Start-Sleep -Seconds 2
}
Write-Output "not found advertising: is the board on and not connected elsewhere?"
exit 2

