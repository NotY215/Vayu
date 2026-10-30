# tests/diagnose_baseline.ps1
#
# Captures stderr from the four baseline failures that run_all.ps1 reports.
# Run this and paste the output back — I cannot fix these without seeing
# the actual error. exceptions.vyu is expected to be fixed by the Phase
# 0.5b patch in VM.cpp; the other three need diagnosis.

param(
    [string]$Configuration = "x64-Release"
)

$ErrorActionPreference = "Continue"
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot  = Split-Path -Parent $scriptDir
Set-Location $repoRoot

$candidates = @(
    "build\$Configuration\bin\vayuc.exe",
    "build\x64-Release\bin\vayuc.exe",
    "build\x64-debug\bin\vayuc.exe"
)
$vayuc = $null
foreach ($c in $candidates) { if (Test-Path $c) { $vayuc = $c; break } }
if (-not $vayuc) {
    Write-Host "FATAL: vayuc.exe not found under build\" -ForegroundColor Red
    exit 2
}
Write-Host "Using: $vayuc" -ForegroundColor Cyan
Write-Host ""

# Cases: (file, mode, description)
$cases = @(
    @{ file = "examples\classes.vyu";      mode = "";     desc = "classes.vyu --run (baseline interp failure)" },
    @{ file = "examples\collections.vyu";  mode = "";     desc = "collections.vyu --run (baseline interp failure)" },
    @{ file = "examples\exceptions.vyu";   mode = "--vm"; desc = "exceptions.vyu --vm (should now be fixed by Phase 0.5b)" },
    @{ file = "examples\generators.vyu";   mode = "--vm"; desc = "generators.vyu --vm (baseline VM 0xC0000409)" }
)

foreach ($c in $cases) {
    Write-Host "================================================================="
    Write-Host ("  " + $c.desc)
    Write-Host "================================================================="
    Write-Host ("  Command: vayuc " + $c.file + $(if ($c.mode) { " " + $c.mode } else { "" }))
    Write-Host ""

    $outTmp = [System.IO.Path]::GetTempFileName()
    $errTmp = [System.IO.Path]::GetTempFileName()
    $argList = @($c.file)
    if ($c.mode) { $argList += $c.mode }

    try {
        & $vayuc @argList 1> $outTmp 2> $errTmp
        $rc = $LASTEXITCODE
    } catch {
        $rc = -999
    }

    $out = ""
    $err = ""
    try { $out = Get-Content $outTmp -Raw -ErrorAction SilentlyContinue } catch {}
    try { $err = Get-Content $errTmp -Raw -ErrorAction SilentlyContinue } catch {}
    Remove-Item -Force $outTmp, $errTmp -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = "" }
    if ($null -eq $err) { $err = "" }

    Write-Host ("  Exit code: " + $rc) -ForegroundColor $(if ($rc -eq 0) { "Green" } else { "Red" })
    Write-Host ""
    Write-Host "  --- stderr ---" -ForegroundColor Yellow
    if ($err.Length -gt 0) {
        Write-Host $err -ForegroundColor DarkYellow
    } else {
        Write-Host "  (empty)" -ForegroundColor DarkGray
    }
    Write-Host ""
    Write-Host "  --- stdout (last 200 chars) ---" -ForegroundColor Yellow
    if ($out.Length -gt 200) {
        Write-Host ("  ..." + $out.Substring($out.Length - 200)) -ForegroundColor DarkGray
    } elseif ($out.Length -gt 0) {
        Write-Host $out -ForegroundColor DarkGray
    } else {
        Write-Host "  (empty)" -ForegroundColor DarkGray
    }
    Write-Host ""
}

Write-Host "================================================================="
Write-Host "Paste the four blocks above back for the remaining Phase 0.5b work."
Write-Host "================================================================="