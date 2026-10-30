# tests/vib_baseline.ps1
#
# Phase 0/1/2/3 baseline harness for VIB Integrator.
#
# Verifies:
#   1. Interpreter (--run) and VM (--vm) produce identical stdout on curated fixtures.
#   2. Native-path flags (--native, --native-out, --emit-runtime) fail with
#      capability-derived messages; --dump-ir succeeds on Phase-2 subset and
#      fails cleanly on non-lowered constructs; --native executes Phase-3
#      arithmetic subset through VIR.
#   3. --vib-status reports every expected feature row.
#   4. No .md file is modified during the run.
#
# Curated fixture notes:
#   - classes.vyu, collections.vyu, exceptions.vyu, generators.vyu are
#     intentionally excluded: they fail in the interpreter or VM before
#     any VIB code runs (baseline project issues outside VIB scope).
#   - See Roadmap.md "Phase 0.5" for the deferred baseline-fix work.

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
    "build\x64-release\bin\vayuc.exe",
    "build\x64-debug\bin\vayuc.exe"
)
$vayuc = $null
foreach ($c in $candidates) { if (Test-Path $c) { $vayuc = $c; break } }
if (-not $vayuc) {
    Write-Host "FATAL: vayuc.exe not found under build\. Build Vayu first." -ForegroundColor Red
    exit 2
}
Write-Host "Using compiler: $vayuc" -ForegroundColor Cyan

# ---- Snapshot .md hashes before anything runs ------------------------------
$mdRoots = @(".", "docs", "VIB", "examples", "Legal")
$mdFiles = @()
foreach ($root in $mdRoots) {
    if (Test-Path $root) {
        $mdFiles += Get-ChildItem -Path $root -Filter "*.md" -File -Recurse `
                        -ErrorAction SilentlyContinue
    }
}
$mdBefore = @{}
foreach ($f in $mdFiles) {
    $mdBefore[$f.FullName] = (Get-FileHash -Algorithm SHA256 $f.FullName).Hash
}
Write-Host "Snapshot: $($mdBefore.Count) .md file(s)." -ForegroundColor DarkGray

$passes   = 0
$failures = 0

function Invoke-Cap([string[]]$cliArgs) {
    $outTmp = [System.IO.Path]::GetTempFileName()
    $errTmp = [System.IO.Path]::GetTempFileName()
    & $vayuc @cliArgs 1> $outTmp 2> $errTmp
    $rc  = $LASTEXITCODE
    $out = Get-Content $outTmp -Raw -ErrorAction SilentlyContinue
    $err = Get-Content $errTmp -Raw -ErrorAction SilentlyContinue
    Remove-Item -Force $outTmp, $errTmp -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = "" }
    if ($null -eq $err) { $err = "" }
    return @{ rc = $rc; out = $out; err = $err }
}

# ---- 1/4 Interpreter vs VM parity -----------------------------------------
Write-Host ""
Write-Host "[1/4] Interpreter vs VM parity on curated fixtures" -ForegroundColor Cyan

# Curated fixtures that pass identically under --run and --vm today.
# classes.vyu, collections.vyu, exceptions.vyu, and generators.vyu are
# intentionally excluded: they fail in the interpreter or VM before any
# VIB code runs. Those are pre-existing project issues outside VIB scope.
$fixtures = @(
    "vm_arith.vyu", "vm_flow.vyu", "vm_fn.vyu",
    "vm_collections.vyu", "vm_exceptions.vyu", "vm_oop.vyu",
    "generics.vyu"
)

foreach ($name in $fixtures) {
    $rel = "examples\$name"
    if (-not (Test-Path $rel)) {
        Write-Host ("  [SKIP] {0} (missing)" -f $name) -ForegroundColor DarkGray
        continue
    }
    $tw = Invoke-Cap @($rel)
    $vm = Invoke-Cap @($rel, "--vm")
    if ($tw.rc -ne 0) {
        Write-Host ("  [FAIL] {0} (--run rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        $failures++; continue
    }
    if ($vm.rc -ne 0) {
        Write-Host ("  [FAIL] {0} (--vm rc={1})" -f $name, $vm.rc) -ForegroundColor Red
        $failures++; continue
    }
    if ($tw.out -ne $vm.out) {
        Write-Host ("  [FAIL] {0} (stdout differs)" -f $name) -ForegroundColor Red
        $failures++; continue
    }
    Write-Host ("  [ok]   {0} (--run == --vm)" -f $name) -ForegroundColor Green
    $passes++
}

# ---- 2/4 Native-path flags -----------------------------------------------
Write-Host ""
Write-Host "[2/4] Native-path flags" -ForegroundColor Cyan

$probe   = "examples\generics.vyu"
$tmpExe  = [System.IO.Path]::GetTempFileName() + ".exe"

$mustFail = @(
    @{ label = "--native";       args = @($probe, "--native");              expect = "NativeCodegen" },
    @{ label = "--native-out";   args = @($probe, "--native-out", $tmpExe); expect = "NativeCodegen" },
    @{ label = "--emit-runtime"; args = @("--emit-runtime", $tmpExe);       expect = "RuntimeEmission" }
)
foreach ($c in $mustFail) {
    $r = Invoke-Cap $c.args
    if ($r.rc -eq 0) {
        Write-Host ("  [FAIL] {0} unexpectedly succeeded" -f $c.label) -ForegroundColor Red
        $failures++; continue
    }
    if ($r.err -notmatch $c.expect) {
        Write-Host ("  [FAIL] {0} missing '{1}' (stderr: {2})" -f $c.label, $c.expect, $r.err) -ForegroundColor Red
        $failures++; continue
    }
    Write-Host ("  [ok]   {0} reports {1}" -f $c.label, $c.expect) -ForegroundColor Green
    $passes++
}
if (Test-Path $tmpExe) { Remove-Item -Force $tmpExe }

# --dump-ir must fail on a non-trivial program with the expected phase
$r = Invoke-Cap @($probe, "--dump-ir")
if ($r.rc -eq 0) {
    Write-Host "  [FAIL] --dump-ir unexpectedly succeeded on $probe" -ForegroundColor Red
    $failures++
} elseif ($r.err -notmatch "planned Phase") {
    Write-Host ("  [FAIL] --dump-ir stderr missing 'planned Phase' (stderr: {0})" -f $r.err) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir rejects non-trivial program with planned-phase diagnostic" -ForegroundColor Green
    $passes++
}

# --dump-ir must succeed on a pass-only program
$passFile = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "vayu_phase2_pass.vyu")
[System.IO.File]::WriteAllText($passFile, "pass`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($passFile, "--dump-ir")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dump-ir on pass-only exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "module") {
    Write-Host "  [FAIL] --dump-ir on pass-only missing 'module'" -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "Const") {
    Write-Host "  [FAIL] --dump-ir on pass-only missing 'Const'" -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "Ret") {
    Write-Host "  [FAIL] --dump-ir on pass-only missing 'Ret'" -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir on pass-only prints valid VIR module" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $passFile -ErrorAction SilentlyContinue

# --native must execute a Phase-3 subset program through VIR.
$arithFile = [System.IO.Path]::Combine([System.IO.Path]::GetTempPath(), "vayu_phase3_arith.vyu")
[System.IO.File]::WriteAllText($arithFile,
    "x = 1 + 2 * 3`nx = x - 1`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($arithFile, "--native")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --native on arithmetic-only exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --native executes arithmetic-only program via VIR" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $arithFile -ErrorAction SilentlyContinue

# --dump-ir on the same Phase-3 program should now succeed and print VIR.
$r = Invoke-Cap @($arithFile, "--dump-ir")
# (recreated for the dump-ir check since we removed above)
[System.IO.File]::WriteAllText($arithFile,
    "x = 1 + 2 * 3`nx = x - 1`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($arithFile, "--dump-ir")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dump-ir on arithmetic-only exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "Add" -or $r.out -notmatch "Mul" -or $r.out -notmatch "Sub") {
    Write-Host "  [FAIL] --dump-ir on arithmetic-only missing Add/Mul/Sub" -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir on arithmetic-only prints VIR with Add/Mul/Sub" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $arithFile -ErrorAction SilentlyContinue

# ---- 3/4 --vib-status shape -----------------------------------------------
Write-Host ""
Write-Host "[3/4] --vib-status reports expected feature rows" -ForegroundColor Cyan

$status = Invoke-Cap @("--vib-status")
if ($status.rc -ne 0) {
    Write-Host "  [FAIL] --vib-status exited $($status.rc)" -ForegroundColor Red
    $failures++
} else {
    $expectedRows = @(
        "BasicArithmetic\s+Supported\s+Deferred\s+3\b",
        "Strings\s+NotSupported\s+Deferred\s+4\b",
        "Functions\s+NotSupported\s+Deferred\s+5\b",
        "ControlFlow\s+NotSupported\s+Deferred\s+6\b",
        "Collections\s+NotSupported\s+Deferred\s+7\b",
        "Modules\s+NotSupported\s+Deferred\s+8\b",
        "Exceptions\s+NotSupported\s+Deferred\s+10\b",
        "Classes\s+NotSupported\s+Deferred\s+10\b",
        "Generics\s+NotSupported\s+Deferred\s+10\b",
        "Generators\s+NotSupported\s+Deferred\s+10\b",
        "Pointers\s+NotSupported\s+Deferred\s+10\b",
        "VIR\s+Supported\s+Deferred\s+2\b",
        "Verifier\s+Supported\s+Deferred\s+2\b",
        "RuntimeEmission\s+Scaffolded\s+Deferred\s+12\b",
        "NativeCodegen\s+Scaffolded\s+Deferred\s+12\b",
        "VyePackaging\s+Deferred\s+Deferred\s+2\b",
        "ArtifactCache\s+Deferred\s+Deferred\s+11\b",
        "IncrementalBuild\s+Deferred\s+Deferred\s+11\b",
        "SelfHosting\s+Deferred\s+Deferred\s+14\b"
    )
    foreach ($row in $expectedRows) {
        if ($status.out -match $row) {
            Write-Host ("  [ok]   {0}" -f $row) -ForegroundColor Green
            $passes++
        } else {
            Write-Host ("  [FAIL] missing row: {0}" -f $row) -ForegroundColor Red
            $failures++
        }
    }
}

# ---- 4/4 .md files unchanged ----------------------------------------------
Write-Host ""
Write-Host "[4/4] No .md file modified during the run" -ForegroundColor Cyan
$mdChanged = 0
foreach ($f in $mdFiles) {
    if (-not (Test-Path $f.FullName)) {
        Write-Host ("  [FAIL] .md removed: {0}" -f $f.FullName) -ForegroundColor Red
        $mdChanged++; continue
    }
    $h = (Get-FileHash -Algorithm SHA256 $f.FullName).Hash
    if ($h -ne $mdBefore[$f.FullName]) {
        Write-Host ("  [FAIL] .md modified: {0}" -f $f.FullName) -ForegroundColor Red
        $mdChanged++
    }
}
if ($mdChanged -eq 0) {
    Write-Host ("  [ok]   all {0} .md file(s) unchanged" -f $mdBefore.Count) -ForegroundColor Green
    $passes++
} else {
    $failures += $mdChanged
}

# ---- Summary --------------------------------------------------------------
Write-Host ""
Write-Host "---------------------------------------------"
Write-Host ("Pass: {0}" -f $passes) -ForegroundColor Green
if ($failures -gt 0) {
    Write-Host ("Fail: {0}" -f $failures) -ForegroundColor Red
    exit 1
} else {
    Write-Host "Fail: 0" -ForegroundColor Gray
    exit 0
}