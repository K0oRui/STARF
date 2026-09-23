# Run with 32-bit PowerShell for the x86 DLL, or 64-bit PowerShell for x64.
# Plays three short sounds; isolates settings and achievement saves in TEMP.
param([string]$Dll, [string]$Sound)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([System.IO.Path]::GetTempPath()) ('STAR-audio-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path "$root\STAR\Sounds" -Force | Out-Null
Copy-Item -LiteralPath $Dll -Destination "$root\steam_api.dll"
Copy-Item -LiteralPath $Sound -Destination "$root\STAR\Sounds\achievement.wav"
Set-Content "$root\STAR\overlay.star" "enabled=false`nplay_sound=true" -Encoding ASCII
Set-Content "$root\STAR\steam_appid.txt" '480' -Encoding ASCII
$env:APPDATA = $root
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class AudioSmoke {
 [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] public static extern IntPtr LoadLibrary(string path);
 [DllImport("kernel32", CharSet=CharSet.Ansi)] public static extern IntPtr GetProcAddress(IntPtr module, string name);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)] public delegate bool Init();
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] [return: MarshalAs(UnmanagedType.I1)] public delegate bool Unlock(IntPtr self, [MarshalAs(UnmanagedType.LPStr)] string name);
 [UnmanagedFunctionPointer(CallingConvention.Cdecl)] public delegate void Shutdown();
}
"@
$module = [AudioSmoke]::LoadLibrary("$root\steam_api.dll")
if ($module -eq [IntPtr]::Zero) { throw 'LoadLibrary failed' }
$init = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([AudioSmoke]::GetProcAddress($module,'SteamAPI_Init'),[AudioSmoke+Init])
$unlock = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([AudioSmoke]::GetProcAddress($module,'SteamAPI_ISteamUserStats_SetAchievement'),[AudioSmoke+Unlock])
$shutdown = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer([AudioSmoke]::GetProcAddress($module,'SteamAPI_Shutdown'),[AudioSmoke+Shutdown])
if (-not $init.Invoke()) { throw 'Init failed' }
1..3 | ForEach-Object { if (-not $unlock.Invoke([IntPtr]::Zero, "audio_test_$_")) { throw 'Unlock failed' } }
Start-Sleep -Seconds 4
$shutdown.Invoke()
$log = Get-Content "$root\STAR\star.log" -Raw
$log -split "`n" | Where-Object { $_ -match 'Sound:' }
$count = ([regex]::Matches($log,'Sound: playback completed:')).Count
if ($count -ne 3) { throw "Expected 3 completed sounds, got $count. Log: $root\STAR\star.log" }
Write-Output 'PASS: three overlapping notification sounds completed'
