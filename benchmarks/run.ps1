# benchmarks/run.ps1
# Phase 25.0 - cross-language benchmark runner.
# Builds once, runs N times per program, prints a table.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -Runs 10
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -IncludeSlow

param(
    [int]$Runs = 5,
    [switch]$IncludeSlow
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root

# --- Toolchain discovery ---------------------------------------------------

$vayuc = Join-Path $repo 'build\x64-debug\bin\vayuc.exe'
if (-not (Test-Path $vayuc)) {
    $cmd = Get-Command vayuc.exe -ErrorAction SilentlyContinue
    if ($cmd) { $vayuc = $cmd.Source } else { $vayuc = $null }
}
$gpp    = (Get-Command g++    -ErrorAction SilentlyContinue).Source
$javac  = (Get-Command javac  -ErrorAction SilentlyContinue).Source
$java   = (Get-Command java   -ErrorAction SilentlyContinue).Source
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = (Get-Command python3 -ErrorAction SilentlyContinue).Source }

Write-Host "== toolchains =="
Write-Host ("  vayuc  = " + ($vayuc  ?? '(not found)'))
Write-Host ("  g++    = " + ($gpp    ?? '(not found)'))
Write-Host ("  javac  = " + ($javac  ?? '(not found)'))
Write-Host ("  java   = " + ($java   ?? '(not found)'))
Write-Host ("  python = " + ($python ?? '(not found)'))
Write-Host ""

# --- Build -----------------------------------------------------------------

$programs = @('arith', 'fib', 'startup')

Write-Host "== building =="

if ($gpp) {
    foreach ($p in $programs) {
        $src = Join-Path $root "cpp\$p.cpp"
        $out = Join-Path $root "cpp\$p.exe"
        if (Test-Path $src) {
            & $gpp -O3 -march=native $src -o $out 2>$null
        }
    }
}
if ($javac) {
    foreach ($cls in @('Arith', 'Fib', 'Startup')) {
        $src = Join-Path $root "java\$cls.java"
        if (Test-Path $src) {
            & $javac -d (Join-Path $root 'java') $src 2>$null
        }
    }
}
if ($vayuc) {
    foreach ($p in $programs) {
        $src = Join-Path $root "vayu\$p.vyu"
        $out = Join-Path $root "vayu\$p.exe"
        if (Test-Path $src) {
            & $vayuc $src --native-out $out 2>$null | Out-Null
        }
    }
}

Write-Host ""

# --- Timing ----------------------------------------------------------------

function Time-Best {
    param([scriptblock]$Action, [int]$N = 5)
    $best = [double]::MaxValue
    for ($i = 0; $i -lt $N; $i++) {
        $sw = [Diagnostics.Stopwatch]::StartNew()
        try { & $Action 2>&1 | Out-Null } catch { }
        $sw.Stop()
        $ms = $sw.Elapsed.TotalMilliseconds
        if ($ms -lt $best) { $best = $ms }
    }
    return $best
}

function Report {
    param([string]$Label, [double]$Ms)
    "{0,-16} {1,12:N1} ms" -f $Label, $Ms
}

function Run-Block {
    param([string]$Name, [string]$JavaClass)
    Write-Host ("--- " + $Name + " ---")

    if ($gpp -and (Test-Path (Join-Path $root "cpp\$Name.exe"))) {
        $exe = Join-Path $root "cpp\$Name.exe"
        Report "cpp" (Time-Best { & $exe } $Runs)
    }
    if ($java -and $JavaClass) {
        Report "java" (Time-Best { & $java -cp (Join-Path $root 'java') $JavaClass } $Runs)
    }
    if ($python -and (Test-Path (Join-Path $root "python\$Name.py"))) {
        $py = Join-Path $root "python\$Name.py"
        Report "python" (Time-Best { & $python $py } $Runs)
    }
    if ($vayuc -and (Test-Path (Join-Path $root "vayu\$Name.exe"))) {
        $exe = Join-Path $root "vayu\$Name.exe"
        Report "vayu-native" (Time-Best { & $exe } $Runs)
    }
    if ($vayuc -and (Test-Path (Join-Path $root "vayu\$Name.vyu"))) {
        $vyu = Join-Path $root "vayu\$Name.vyu"
        Report "vayu-vm" (Time-Best { & $vayuc $vyu --vm } $Runs)
        if ($IncludeSlow -or $Name -ne 'fib') {
            Report "vayu-tree" (Time-Best { & $vayuc $vyu } $Runs)
        } else {
            Report "vayu-tree" 0
            Write-Host "                 (skipped: pass -IncludeSlow to run)"
        }
    }
    Write-Host ""
}

Write-Host ("== results (best of " + $Runs + ") ==")
Write-Host ""

Run-Block 'arith'   'Arith'
Run-Block 'fib'     'Fib'
Run-Block 'startup' 'Startup'

Write-Host "notes:"
Write-Host "  cpp          g++ -O3 -march=native"
Write-Host "  java         default JIT"
Write-Host "  python       CPython"
Write-Host "  vayu-native  vayuc --native-out, gcc -O2"
Write-Host "  vayu-vm      vayuc --vm (includes frontend)"
Write-Host "  vayu-tree    vayuc (includes frontend)"