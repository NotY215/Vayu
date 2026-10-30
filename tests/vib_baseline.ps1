# tests/vib_baseline.ps1
#
# Phase 0/1/2/3/4/5 + 2A.2/2B.1/2B.2 baseline harness for VIB Integrator.
#
# Verifies:
#   1. Interpreter (--run) and VM (--vm) produce identical stdout on curated fixtures.
#   2. Native-path flags fail with capability-derived messages; --dump-ir succeeds
#      on the Phase 2/3/4 subset and fails cleanly on constructs beyond Phase 5.
#   3. --vib-status reports every expected feature row.
#   4. No .md file is modified during the run.
#   5. .vye writer/reader round-trip (Phase 2A.2 / 2B.1) — opt-in.
#   6. Dispatch adoption (Phase 2C.1/2C.2) — opt-in.
#   7. Artifact cache (Phase 2C.3) — opt-in.
#
# Curated fixture notes:
#   - classes.vyu, collections.vyu, exceptions.vyu, generators.vyu are
#     intentionally excluded from [1/4]: they fail in the interpreter or VM
#     before any VIB code runs (baseline project issues outside VIB scope).
#   - See Roadmap.md "Phase 0.5" for the deferred baseline-fix work.
#   - exceptions.vyu is now fixed by the VMRuntimeError catch (Phase 0.5b).
#     The remaining three await the diagnostic script's stderr.

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

# ---- 1/7 Interpreter vs VM parity -----------------------------------------
Write-Host ""
Write-Host "[1/7] Interpreter vs VM parity on curated fixtures" -ForegroundColor Cyan

# exceptions.vyu has pre-existing --run vs --vm stdout parity drift (Phase 0.5b,
# outside VIB). Exception coverage remains via vm_exceptions.vyu.
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

# ---- 2/7 Native-path flags -----------------------------------------------
Write-Host ""
Write-Host "[2/7] Native-path flags" -ForegroundColor Cyan

# Probe that must still reject on Integrator (classes = Phase 10).
$rejectProbe = [System.IO.Path]::GetTempPath() + "vayu_reject_phase10.vyu"
[System.IO.File]::WriteAllText($rejectProbe, "class C:`n    x: int`n    def __init__(self) -> None:`n        self.x = 1`nprint(C().x)`n", (New-Object System.Text.UTF8Encoding($false)))
$probe   = $rejectProbe
$tmpExe  = [System.IO.Path]::GetTempFileName() + ".exe"

$mustFail = @(
    @{ label = "--native";       args = @($probe, "--native");
       expect = "planned Phase|NativeCodegen|Classes|Phase 10" },
    @{ label = "--native-out";   args = @($probe, "--native-out", $tmpExe);
       expect = "planned Phase|NativeCodegen|Classes|Phase 10" },
    @{ label = "--emit-runtime"; args = @("--emit-runtime", $tmpExe);
       expect = "RuntimeEmission" }
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
$passFile = [System.IO.Path]::GetTempPath() + "vayu_phase2_pass.vyu"
[System.IO.File]::WriteAllText($passFile, "pass`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($passFile, "--dump-ir")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dump-ir on pass-only exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "module" -or $r.out -notmatch "Const" -or $r.out -notmatch "Ret") {
    Write-Host "  [FAIL] --dump-ir on pass-only missing expected tokens" -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir on pass-only prints valid VIR module" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $passFile -ErrorAction SilentlyContinue

# --native on arithmetic-only program
$arithFile = [System.IO.Path]::GetTempPath() + "vayu_phase3_arith.vyu"
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

# --dump-ir on arithmetic-only
$r = Invoke-Cap @($arithFile, "--dump-ir")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dump-ir on arithmetic-only exited {0}" -f $r.rc) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "Add" -or $r.out -notmatch "Mul" -or $r.out -notmatch "Sub") {
    Write-Host "  [FAIL] --dump-ir on arithmetic-only missing Add/Mul/Sub" -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir on arithmetic-only prints VIR with Add/Mul/Sub" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $arithFile -ErrorAction SilentlyContinue

# --native on string-only program
$strFile = [System.IO.Path]::GetTempPath() + "vayu_phase4_str.vyu"
[System.IO.File]::WriteAllText($strFile,
    "s = `"a`" + `"b`"`nt = s == `"ab`"`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($strFile, "--native")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --native on string-only exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --native executes string-only program via VIR" -ForegroundColor Green
    $passes++
}
$r = Invoke-Cap @($strFile, "--dump-ir")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dump-ir on string-only exited {0}" -f $r.rc) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "StrConcat" -or $r.out -notmatch "StrEq") {
    Write-Host "  [FAIL] --dump-ir on string-only missing StrConcat/StrEq" -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dump-ir on string-only prints VIR with StrConcat/StrEq" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $strFile -ErrorAction SilentlyContinue

# --native on a user-defined function with print
$fnFile = [System.IO.Path]::GetTempPath() + "vayu_phase5_fn.vyu"
[System.IO.File]::WriteAllText($fnFile,
    "def greet(name: str) -> None:`n" +
    "    print(`"hi `" + name)`n" +
    "`n" +
    "greet(`"world`")`n",
    (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($fnFile, "--native")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --native on function program exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "hi world") {
    Write-Host ("  [FAIL] --native on function program missing 'hi world' (got: {0})" -f $r.out) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --native executes user-defined function + print" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $fnFile -ErrorAction SilentlyContinue

# --native on a function with return value (Phase 5.2b plumbing)
$retFile = [System.IO.Path]::GetTempPath() + "vayu_phase5_ret.vyu"
[System.IO.File]::WriteAllText($retFile,
    "def add(a: int, b: int) -> int:`n" +
    "    return a + b`n" +
    "`n" +
    "x = add(2, 3)`n" +
    "print(x)`n",
    (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($retFile, "--native")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --native on function-with-return exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif ($r.out -notmatch "5") {
    Write-Host ("  [FAIL] --native on function-with-return missing '5' (got: {0})" -f $r.out) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --native plumbs function return value (5)" -ForegroundColor Green
    $passes++
}
Remove-Item -Force $retFile -ErrorAction SilentlyContinue

# ---- 3/7 --vib-status ----------------------------------------------------
Write-Host ""
Write-Host "[3/7] --vib-status reports expected feature rows" -ForegroundColor Cyan

$status = Invoke-Cap @("--vib-status")
if ($status.rc -ne 0) {
    Write-Host "  [FAIL] --vib-status exited $($status.rc)" -ForegroundColor Red
    $failures++
} else {
    $expectedRows = @(
        "BasicArithmetic\s+Supported\s+Deferred\s+3\b",
        "Strings\s+Supported\s+Deferred\s+4\b",
        "Functions\s+Supported\s+Deferred\s+5\b",
        "ControlFlow\s+Supported\s+Deferred\s+6\b",
        "Collections\s+Supported\s+Deferred\s+7\b",
        "Modules\s+Supported\s+Deferred\s+8\b",
        "Exceptions\s+NotSupported\s+Deferred\s+10\b",
        "Classes\s+NotSupported\s+Deferred\s+10\b",
        "Generics\s+NotSupported\s+Deferred\s+10\b",
        "Generators\s+NotSupported\s+Deferred\s+10\b",
        "Pointers\s+NotSupported\s+Deferred\s+10\b",
        "VIR\s+Supported\s+Deferred\s+2\b",
        "Verifier\s+Supported\s+Deferred\s+2\b",
        "RuntimeEmission\s+Scaffolded\s+Deferred\s+12\b",
        "NativeCodegen\s+Scaffolded\s+Deferred\s+12\b",
        "VyePackaging\s+Supported\s+Supported\s+2\b",
        "ArtifactCache\s+(Deferred|Scaffolded|Supported)\s+(Deferred|Scaffolded|Supported)\s+11\b",
        "IncrementalBuild\s+(Deferred|Scaffolded|Supported)\s+(Deferred|Scaffolded|Supported)\s+11\b",
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

# ---- 4/7 .md files unchanged ---------------------------------------------
Write-Host ""
Write-Host "[4/7] No .md file modified during the run" -ForegroundColor Cyan
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

# ---- 5/7 .vye writer/reader round-trip (Phase 2A.2 / 2B.1) ---------------
Write-Host ""
Write-Host "[5/7] .vye writer/reader round-trip (opt-in)" -ForegroundColor Cyan

$vyeFile = [System.IO.Path]::GetTempPath() + "vayu_phase2a_test.vye"
Remove-Item -Force $vyeFile -ErrorAction SilentlyContinue
$arithForVye = [System.IO.Path]::GetTempPath() + "vayu_phase2a_src.vyu"
[System.IO.File]::WriteAllText($arithForVye, "x = 1 + 2`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($arithForVye, "--emit-vye", $vyeFile)
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --emit-vye exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} elseif (-not (Test-Path $vyeFile)) {
    Write-Host "  [FAIL] --emit-vye did not create file" -ForegroundColor Red
    $failures++
} else {
    $sz = (Get-Item $vyeFile).Length
    if ($sz -lt 8) {
        Write-Host ("  [FAIL] .vye too small ({0} bytes)" -f $sz) -ForegroundColor Red
        $failures++
    } else {
        Write-Host ("  [ok]   --emit-vye wrote {0} bytes" -f $sz) -ForegroundColor Green
        $passes++
    }
    # run the .vye
    $r = Invoke-Cap @("--run-vye", $vyeFile)
    if ($r.rc -ne 0) {
        Write-Host ("  [FAIL] --run-vye exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
        $failures++
    } else {
        Write-Host "  [ok]   --run-vye executed a valid .vye" -ForegroundColor Green
        $passes++
    }
    # corrupt and expect rejection
    $corrupt = [System.IO.Path]::GetTempPath() + "vayu_phase2a_corrupt.vye"
    $bytes = [System.IO.File]::ReadAllBytes($vyeFile)
    $bytes[0] = 0xFF
    [System.IO.File]::WriteAllBytes($corrupt, $bytes)
    $r = Invoke-Cap @("--run-vye", $corrupt)
    if ($r.rc -eq 0) {
        Write-Host "  [FAIL] --run-vye accepted corrupt .vye" -ForegroundColor Red
        $failures++
    } else {
        Write-Host "  [ok]   --run-vye rejects corrupt .vye" -ForegroundColor Green
        $passes++
    }
    Remove-Item -Force $corrupt -ErrorAction SilentlyContinue
}
Remove-Item -Force $vyeFile, $arithForVye -ErrorAction SilentlyContinue

# ---- 6/7 Dispatch adoption (Phase 2C.1/2C.2) ----------------------------
Write-Host ""
Write-Host "[6/7] Dispatch adoption (opt-in)" -ForegroundColor Cyan

$dispatchProbe = "examples\vm_arith.vyu"
$r = Invoke-Cap @($dispatchProbe, "--dispatch=integrator")
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --dispatch=integrator on {0} exited {1} (stderr: {2})" -f $dispatchProbe, $r.rc, $r.err) -ForegroundColor Red
    $failures++
} else {
    Write-Host "  [ok]   --dispatch=integrator runs through Integrator dispatch" -ForegroundColor Green
    $passes++
}

# ---- 7/7 Artifact cache (Phase 2C.3) ------------------------------------
Write-Host ""
Write-Host "[7/7] Artifact cache (opt-in)" -ForegroundColor Cyan

$cacheDir = [System.IO.Path]::GetTempPath() + "vayu_phase2c_cache"
Remove-Item -Recurse -Force $cacheDir -ErrorAction SilentlyContinue
$arithForCache = [System.IO.Path]::GetTempPath() + "vayu_phase2c_src.vyu"
[System.IO.File]::WriteAllText($arithForCache, "x = 1 + 2`n", (New-Object System.Text.UTF8Encoding($false)))
$r = Invoke-Cap @($arithForCache, "--native", "--enable-cache", "--cache-dir", $cacheDir)
if ($r.rc -ne 0) {
    Write-Host ("  [FAIL] --native --enable-cache exited {0} (stderr: {1})" -f $r.rc, $r.err) -ForegroundColor Red
    $failures++
} else {
    $entries = 0
    if (Test-Path $cacheDir) {
        $entries = @(Get-ChildItem $cacheDir -File -Recurse -ErrorAction SilentlyContinue).Count
    }
    if ($entries -gt 0) {
        Write-Host ("  [ok]   cache populated ({0} file(s))" -f $entries) -ForegroundColor Green
        $passes++
    } else {
        Write-Host "  [FAIL] cache not populated" -ForegroundColor Red
        $failures++
    }
}
Remove-Item -Recurse -Force $cacheDir -ErrorAction SilentlyContinue
Remove-Item -Force $arithForCache -ErrorAction SilentlyContinue

# ---- Summary -------------------------------------------------------------
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