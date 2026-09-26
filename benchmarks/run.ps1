# benchmarks/run.ps1
# Cross-language benchmark runner.  Prints one table at the end.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -Quick
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -IncludeSlow
#   powershell -ExecutionPolicy Bypass -File benchmarks\run.ps1 -Runs 3

param(
    [int]$Runs = 3,
    [switch]$Quick,        # only arith / fib / startup
    [switch]$IncludeSlow,  # include tree-walk for fib
    [switch]$BothBuilds    # show debug and release vayuc side by side
)

$ErrorActionPreference = 'Continue'
$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = Split-Path -Parent $root

# --- Toolchain discovery ---------------------------------------------------

$vayucDbg = Join-Path $repo 'build\x64-debug\bin\vayuc.exe'
if (-not (Test-Path $vayucDbg)) {
    $c = Get-Command vayuc.exe -ErrorAction SilentlyContinue
    if ($c) { $vayucDbg = $c.Source } else { $vayucDbg = $null }
}
$vayucRel = Join-Path $repo 'build\x64-release\bin\vayuc.exe'
if (-not (Test-Path $vayucRel)) { $vayucRel = $null }

$gpp    = (Get-Command g++    -ErrorAction SilentlyContinue).Source
$javac  = (Get-Command javac  -ErrorAction SilentlyContinue).Source
$java   = (Get-Command java   -ErrorAction SilentlyContinue).Source
$python = (Get-Command python -ErrorAction SilentlyContinue).Source
if (-not $python) { $python = (Get-Command python3 -ErrorAction SilentlyContinue).Source }

$vayucBuild = if ($vayucRel) { $vayucRel } else { $vayucDbg }

# --- Programs --------------------------------------------------------------

$names = @{
    'arith'         = 'Arith'
    'fib'           = 'Fib'
    'startup'       = 'Startup'
    'matmul'        = 'Matmul'
    'nbody'         = 'Nbody'
    'mandelbrot'    = 'Mandelbrot'
    'string_concat' = 'StringConcat'
    'hashmap'       = 'Hashmap'
    'json_parse'    = 'JsonParse'
    'sort'          = 'Sort'
}
if ($Quick) {
    $programs = @('arith', 'fib', 'startup')
} else {
    $programs = @('arith', 'fib', 'startup', 'matmul', 'nbody', 'mandelbrot',
                  'string_concat', 'hashmap', 'json_parse', 'sort')
}

# --- Build -----------------------------------------------------------------

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
    foreach ($p in $programs) {
        $cls = $names[$p]
        $src = Join-Path $root "java\$cls.java"
        if (Test-Path $src) {
            & $javac -d (Join-Path $root 'java') $src 2>$null
        }
    }
}
if ($vayucBuild) {
    foreach ($p in $programs) {
        $src = Join-Path $root "vayu\$p.vyu"
        $out = Join-Path $root "vayu\$p.exe"
        if (Test-Path $src) {
            & $vayucBuild $src --native-out $out 2>$null | Out-Null
        }
    }
}

# --- Measurement -----------------------------------------------------------

function Measure-Program {
    param(
        [string]$FilePath,
        [string[]]$ArgList,
        [int]$N
    )
    if (-not (Test-Path $FilePath)) { return $null }
    $bestMs  = [double]::MaxValue
    $peakRss = [long]0
    for ($i = 0; $i -lt $N; $i++) {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName               = $FilePath
        $psi.UseShellExecute        = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError  = $true
        $psi.CreateNoWindow         = $true
        if ($ArgList -and $ArgList.Count -gt 0) {
            $psi.Arguments = ($ArgList | ForEach-Object { '"' + $_ + '"' }) -join ' '
        }
        $p = New-Object System.Diagnostics.Process
        $p.StartInfo = $psi

        $sw = [Diagnostics.Stopwatch]::StartNew()
        try { $p.Start() | Out-Null } catch { return $null }
        # Drain both streams synchronously; they close on process exit,
        # so this doubles as a blocking wait.
        $null = $p.StandardOutput.ReadToEnd()
        $null = $p.StandardError.ReadToEnd()
        $p.WaitForExit()
        $sw.Stop()

        $ms = $sw.Elapsed.TotalMilliseconds
        if ($ms -lt $bestMs) { $bestMs = $ms }
        try {
            $ws = $p.PeakWorkingSet64
            if ($ws -gt $peakRss) { $peakRss = $ws }
        } catch { }
        $p.Dispose()
    }
    return @{ Ms = $bestMs; Rss = $peakRss }
}

# --- Run -------------------------------------------------------------------

$rows = New-Object System.Collections.ArrayList

function Add-Row {
    param([string]$Prog, [string]$Lang, $Result)
    if ($null -eq $Result) { return }
    $null = $rows.Add([pscustomobject]@{
        prog = $Prog
        lang = $Lang
        ms   = [math]::Round($Result.Ms, 1)
        mb   = [math]::Round($Result.Rss / 1MB, 1)
    })
}

foreach ($p in $programs) {
    $cls = $names[$p]

    if ($gpp) {
        Add-Row $p 'cpp' (Measure-Program (Join-Path $root "cpp\$p.exe") @() $Runs)
    }
    if ($java) {
        Add-Row $p 'java' (Measure-Program $java `
            @('-cp', (Join-Path $root 'java'), $cls) $Runs)
    }
    if ($python) {
        Add-Row $p 'python' (Measure-Program $python `
            @((Join-Path $root "python\$p.py")) $Runs)
    }
    if ($vayucBuild) {
        Add-Row $p 'vayu-native' (Measure-Program (Join-Path $root "vayu\$p.exe") @() $Runs)
    }

    $vyu = Join-Path $root "vayu\$p.vyu"
    if (Test-Path $vyu) {
        if ($BothBuilds) {
            if ($vayucDbg) {
                Add-Row $p 'vayu-vm-debug' (Measure-Program $vayucDbg @($vyu, '--vm') $Runs)
            }
            if ($vayucRel) {
                Add-Row $p 'vayu-vm-release' (Measure-Program $vayucRel @($vyu, '--vm') $Runs)
            }
        } else {
            if ($vayucBuild) {
                Add-Row $p 'vayu-vm' (Measure-Program $vayucBuild @($vyu, '--vm') $Runs)
            }
        }

        $runTree = $IncludeSlow -or ($p -ne 'fib')
        if ($runTree) {
            if ($BothBuilds) {
                if ($vayucDbg) {
                    Add-Row $p 'vayu-tree-debug' (Measure-Program $vayucDbg @($vyu) $Runs)
                }
                if ($vayucRel) {
                    Add-Row $p 'vayu-tree-release' (Measure-Program $vayucRel @($vyu) $Runs)
                }
            } else {
                if ($vayucBuild) {
                    Add-Row $p 'vayu-tree' (Measure-Program $vayucBuild @($vyu) $Runs)
                }
            }
        }
    }
}

# --- Print the table -------------------------------------------------------

Write-Host ""
Write-Host ("== results (best of " + $Runs + ") ==")
Write-Host ""
"{0,-14} {1,-18} {2,10} {3,10}" -f 'program', 'lang', 'time(ms)', 'rss(MB)'
"{0,-14} {1,-18} {2,10} {3,10}" -f '-------', '----', '--------', '-------' | Write-Host
foreach ($r in $rows) {
    "{0,-14} {1,-18} {2,10:N1} {3,10:N1}" -f $r.prog, $r.lang, $r.ms, $r.mb | Write-Host
}
Write-Host ""
Write-Host "notes:"
Write-Host "  cpp           g++ -O3 -march=native"
Write-Host "  java          default JIT"
Write-Host "  python        CPython"
Write-Host "  vayu-native   gcc -O2"
Write-Host "  vayu-vm       bytecode VM"
Write-Host "  vayu-tree     AST interpreter"