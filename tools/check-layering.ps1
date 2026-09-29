# Fails when the engine's core or ports include anything but the standard library and
# each other. Adapters, OpenVINO, SQLite, Win32 and nlohmann/json belong in adapters/.
#
#   .\tools\check-layering.ps1 [-Root <repo>]
[CmdletBinding()]
param(
    [string]$Root
)

$ErrorActionPreference = 'Stop'
# Windows PowerShell leaves $PSScriptRoot empty in parameter defaults
if (-not $Root) { $Root = Split-Path $PSScriptRoot -Parent }
$domain = Join-Path $Root 'engine\domain'
$allowed = '^\s*#\s*include\s*(<[a-z0-9_]+>|"(core|ports)/[^"]+")'
$reasons = [ordered]@{
    'adapters/'      = 'an adapter'
    'openvino'       = 'OpenVINO'
    'sqlite'         = 'SQLite'
    'nlohmann'       = 'nlohmann/json'
    '\.h[">]|pragma' = 'Win32 or another platform header'
}

$files = @('core', 'ports') | ForEach-Object {
    Get-ChildItem -Path (Join-Path $domain $_) -Recurse -File -Include *.cpp, *.hpp, *.h, *.inl
}
$violations = @($files |
    Select-String -Pattern '^\s*#\s*(include|pragma\s+comment)' |
    Where-Object { $_.Line -notmatch $allowed })

foreach ($v in $violations) {
    $reason = 'not a standard, core or ports header'
    foreach ($pattern in $reasons.Keys) {
        if ($v.Line -match $pattern) { $reason = $reasons[$pattern]; break }
    }
    $path = [IO.Path]::GetRelativePath($Root, $v.Path).Replace('\', '/')
    Write-Host "${path}:$($v.LineNumber): $($v.Line.Trim())  <- $reason"
}

if ($violations.Count -gt 0) {
    Write-Host "$($violations.Count) line(s) break the engine layering"
    exit 1
}
Write-Host "Engine layering holds: $($files.Count) core and ports files include only the standard library, core and ports"
