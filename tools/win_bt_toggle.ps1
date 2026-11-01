# Turn the PC's Bluetooth radio off and on again (wakes up a stuck Windows
# auto-reconnect). Other Bluetooth devices drop for a few seconds.
#   powershell -ExecutionPolicy Bypass -File tools\win_bt_toggle.ps1
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTask = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
function Await($op, [Type]$type) {
    $t = $asTask.MakeGenericMethod($type).Invoke($null, @($op))
    $t.Wait(-1) | Out-Null
    $t.Result
}
[Windows.Devices.Radios.Radio, Windows.System.Devices, ContentType = WindowsRuntime] | Out-Null
$R = [Windows.Devices.Radios.Radio]
Await ($R::RequestAccessAsync()) ([Windows.Devices.Radios.RadioAccessStatus]) | Out-Null
$radios = Await ($R::GetRadiosAsync()) ([System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]])
$bt = $radios | Where-Object { $_.Kind -eq 'Bluetooth' }
foreach ($r in $bt) {
    $s = Await ($r.SetStateAsync('Off')) ([Windows.Devices.Radios.RadioAccessStatus]); Write-Output "off: $s"
    Start-Sleep 3
    $s = Await ($r.SetStateAsync('On')) ([Windows.Devices.Radios.RadioAccessStatus]); Write-Output "on: $s"
}
if (-not $bt) { Write-Output "no Bluetooth radio found" }
