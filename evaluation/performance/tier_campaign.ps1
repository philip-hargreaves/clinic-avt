# Stop-to-last-token across note tiers at 1x: every track through every arm, interleaved by
# track so a warming machine lands on each arm alike. A fresh engine per run, so each record
# carries that arm's load. Writes runs-<tag>.jsonl and logs/<tag>-<track>.log; see tier_report.py.
param(
    [string[]]$Tracks = @("c02m", "c05m", "c10m"),
    [string]$Out = (Join-Path $PSScriptRoot "..\..\build\perf-loop")
)
$ErrorActionPreference = "Continue"
Set-Location (Join-Path $PSScriptRoot "..\..")
$arms = @(
    @{ tag = "tier-4b";        tier = "constrained" },
    @{ tag = "tier-9b";        tier = "default" },
    @{ tag = "tier-35b";       tier = "accuracy" }
)
"campaign $(Get-Date -Format 'yyyy-MM-dd HH:mm')  power: $((Get-CimInstance -Namespace root/cimv2/power -ClassName Win32_PowerPlan -Filter 'IsActive=true' -ErrorAction SilentlyContinue).ElementName)" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
foreach ($track in $Tracks) {
    foreach ($arm in $arms) {
        $env:PERF_NOTE_TIER = $arm.tier
        $env:PERF_TAG = "-" + $arm.tag
        $env:PERF_CYCLES = "1"
        "=== $track / $($arm.tag)  $(Get-Date -Format 'HH:mm:ss')" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
        python evaluation/performance/perf_loop.py 1 $track 2>&1 | Select-Object -Last 3 | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
        $log = Join-Path $Out "logs\engine-001.log"
        if (Test-Path $log) { Copy-Item $log (Join-Path $Out "logs\$($arm.tag)-$track.log") -Force }
        Start-Sleep 5
    }
}
Remove-Item Env:PERF_NOTE_TIER, Env:PERF_TAG -ErrorAction SilentlyContinue
"campaign done $(Get-Date -Format 'HH:mm:ss')" | Tee-Object -FilePath (Join-Path $Out "tier-campaign.log") -Append
