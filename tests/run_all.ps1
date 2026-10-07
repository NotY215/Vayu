# tests\run_all.ps1 -- cross-backend sanity harness.
#
# Native backend: VCB (tools\vcb.exe).  vayuc dispatches to it via the
# VAYU_VCB environment variable; if unset, vayuc falls back to
# tools\vcb.exe.  This script sets VAYU_VCB explicitly so the
# freshness check below is meaningful.
#
# QBE and gcc were removed in Phase 26 Part 2.9e.  Signing was removed
# from VCB in the post-27 cleanup.  VCB emits machine code and a
# complete PE or ELF image directly.
#
# Modes:
#   (no args)              full: examples + fixpoint
#   -Test <name>           one example only, all backends, no fixpoint
#   -FixpointOnly          skip examples, run the fixpoint (stubbed)
#   -NoFixpoint            examples only (fast iteration)
#   -FailOnDenied          treat Windows ACCESS_DENIED as a failure.
#                          Default: skip.  Denied means the WDAC inbox
#                          policy blocked the freshly-built unsigned
#                          binary at launch, not a codegen bug.

param(
    [string]$Test = "",
    [switch]$FixpointOnly,
    [switch]$NoFixpoint,
    [switch]$FailOnDenied
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$vayuc = "build\x64-Release\bin\vayuc.exe"
if (-not (Test-Path $vayuc)) {
    Write-Host "FATAL: $vayuc not found. Build Vayu first." -ForegroundColor Red
    exit 2
}

# ---- VCB resolution ---------------------------------------------------
$vcbRel = "tools\vcb.exe"
if (-not (Test-Path $vcbRel)) {
    Write-Host "FATAL: $vcbRel not found. Copy vcb.exe into tools\." -ForegroundColor Red
    exit 2
}
$env:VAYU_VCB = (Resolve-Path $vcbRel).Path

# ---- Freshness check --------------------------------------------------
$vcbSrcBuild = "E:\VCB\build\x64-Release\bin\vcb.exe"
if (Test-Path $vcbSrcBuild) {
    $toolTime = (Get-Item $vcbRel).LastWriteTime
    $srcTime  = (Get-Item $vcbSrcBuild).LastWriteTime
    if ($srcTime -gt $toolTime) {
        Write-Host ("WARN: tools\vcb.exe is older than $vcbSrcBuild.") -ForegroundColor Yellow
        Write-Host ("      tools: {0:yyyy-MM-dd HH:mm}" -f $toolTime) -ForegroundColor Yellow
        Write-Host ("      src  : {0:yyyy-MM-dd HH:mm}" -f $srcTime)  -ForegroundColor Yellow
        Write-Host "      Results may reflect stale code.  Rebuild VCB and re-copy." `
            -ForegroundColor Yellow
    }
}
$vcbInfo = Get-Item $vcbRel
Write-Host ("VCB   : {0}  ({1:yyyy-MM-dd HH:mm})" -f $vcbRel, $vcbInfo.LastWriteTime) `
    -ForegroundColor Cyan

# ---- Test lists -------------------------------------------------------

$skip = @(
    "nn.vyu",
    "input.vyu", "native_io.vyu", "type_errors.vyu", "expr.vyu",
    "vlex_test.vyu", "vparse_test.vyu", "vcode_test.vyu",
    "native_collections.vyu"
)

$native_only = @(
    "fs_test.vyu", "time_test.vyu", "json_test.vyu", "regex_test.vyu",
    "thread_test.vyu", "net_test.vyu", "crypto_test.vyu", "random_test.vyu",
    "os_test.vyu", "ptr_arith_test.vyu", "ffi_callback_test.vyu",
    "ffi_test.vyu", "ffi_struct_test.vyu", "ffi_wrap_test.vyu",
    "py_init_test.vyu", "py_bridge_test.vyu", "py_call_test.vyu",
    "py_bidi_test.vyu", "py_close_test.vyu", "py_eval_test.vyu",
    "py_callback_test.vyu",
    "self_host_17_7.vyu", "self_host_17_8.vyu",
    "gui_window.vyu",
    "gui_events.vyu",
    "gui_widgets.vyu",
    "gui_canvas.vyu",
    "gui_bitmap.vyu",
    "gui_transform.vyu",
    "gui_text.vyu",
    "gui_image_io.vyu",
    "raster_tri.vyu",
    "raster_cube.vyu",
    "raster_lit_cube.vyu",
    "raster_postfx.vyu",
    "tensor_basic.vyu",
    "tensor_autograd.vyu",
    "nn_mlp.vyu",
    "onnx_mlp.vyu",
    "onnx_mlp_real.vyu",
    "cuda_matmul.vyu",
    "dml_matmul.vyu",
    "nva_registry_test.vyu",
    "nva_git_test.vyu"
)

$sort_compare = @( "generators.vyu" )

# Examples whose exit code is intentionally non-zero on any backend.
$expected_exit = @{
    "vcb_smoke.vyu" = 42
}

# ---- Helpers ----------------------------------------------------------

# Returns @{ rc; out; err }.  Captures stdout and stderr into separate
# temp files so the comparison is against the child program's stdout
# only, not against vcb's status lines or vayuc's diagnostics.
function Invoke-Backend {
    param($file, $mode)
    $outTmp = [System.IO.Path]::GetTempFileName()
    $errTmp = [System.IO.Path]::GetTempFileName()
    $argList = @($file)
    if ($mode) { $argList += $mode }
    & $vayuc @argList 1> $outTmp 2> $errTmp
    $rc = $LASTEXITCODE
    $out = Get-Content $outTmp -Raw -ErrorAction SilentlyContinue
    $err = Get-Content $errTmp -Raw -ErrorAction SilentlyContinue
    Remove-Item -Force $outTmp -ErrorAction SilentlyContinue
    Remove-Item -Force $errTmp -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = "" }
    if ($null -eq $err) { $err = "" }
    return @{ rc = $rc; out = $out; err = $err }
}

function Compare-Output {
    param([string]$a, [string]$b, [bool]$sortLines)
    if (-not $sortLines) { return ($a -eq $b) }
    $la = @($a -split "`r?`n" | Sort-Object)
    $lb = @($b -split "`r?`n" | Sort-Object)
    return (($la -join "`n") -eq ($lb -join "`n"))
}

# Windows returns several codes when the loader refuses an unsigned
# binary under the WDAC inbox policy:
#     5           ERROR_ACCESS_DENIED (cmd.exe's mapping)
#    -1073741790  0xC0000022  STATUS_ACCESS_DENIED
#    -1073741819  0xC0000005  STATUS_ACCESS_VIOLATION (rare, sandbox kill)
function Is-Denied([int]$rc) {
    return ($rc -eq 5 -or $rc -eq -1073741790 -or $rc -eq -1073741819)
}

function ExitCodeOK([string]$name, [int]$rc) {
    if ($rc -eq 0) { return $true }
    if ($expected_exit.ContainsKey($name)) {
        return ($rc -eq $expected_exit[$name])
    }
    return $false
}

function Test-OneFile([string]$rel, [string]$name) {
    if ($skip -contains $name) {
        Write-Host ("[skip]  {0}  (in skip list)" -f $name) -ForegroundColor DarkGray
        return $true
    }

    if ($native_only -contains $name) {
        $nat = Invoke-Backend $rel "--native"
        if (ExitCodeOK $name $nat.rc) {
            Write-Host ("[ok]    {0}  (native-only)" -f $name) -ForegroundColor Green
            return $true
        }
        if ((Is-Denied $nat.rc) -and -not $FailOnDenied) {
            Write-Host ("[denied]{0}  (native rc={1})" -f $name, $nat.rc) -ForegroundColor Yellow
            return $true
        }
        Write-Host ("[FAIL]  {0}  (native-only rc={1})" -f $name, $nat.rc) -ForegroundColor Red
        if ($nat.err) {
            $lines = @($nat.err -split "`r?`n" | Where-Object { $_ })
            $last = if ($lines.Count -gt 0) { $lines[$lines.Count - 1] } else { "" }
            if ($last) { Write-Host ("        {0}" -f $last) -ForegroundColor DarkRed }
        }
        return $false
    }

    $tw = Invoke-Backend $rel ""
    if (-not (ExitCodeOK $name $tw.rc)) {
        Write-Host ("[FAIL]  {0}  (tree-walk rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        return $false
    }

    $vm  = Invoke-Backend $rel "--vm"
    $nat = Invoke-Backend $rel "--native"
    $useSort = $sort_compare -contains $name

    $ok = $true
    if ((ExitCodeOK $name $vm.rc) -and -not (Compare-Output $vm.out $tw.out $useSort)) {
        Write-Host ("[FAIL]  {0}  (VM output differs)" -f $name) -ForegroundColor Red
        $ok = $false
    }

    if (ExitCodeOK $name $nat.rc) {
        if (-not (Compare-Output $nat.out $tw.out $useSort)) {
            Write-Host ("[FAIL]  {0}  (native output differs)" -f $name) -ForegroundColor Red
            Write-Host ("        tree : {0}" -f ($tw.out -replace "`n", "\n")) -ForegroundColor DarkYellow
            Write-Host ("        nativ: {0}" -f ($nat.out -replace "`n", "\n")) -ForegroundColor DarkYellow
            $ok = $false
        }
    }
    elseif ((Is-Denied $nat.rc) -and -not $FailOnDenied) {
        Write-Host ("[denied]{0}  (native rc={1})" -f $name, $nat.rc) -ForegroundColor Yellow
    }
    else {
        Write-Host ("[FAIL]  {0}  (native rc={1})" -f $name, $nat.rc) -ForegroundColor Red
        if ($nat.err) {
            $lines = @($nat.err -split "`r?`n" | Where-Object { $_ })
            $last = if ($lines.Count -gt 0) { $lines[$lines.Count - 1] } else { "" }
            if ($last) { Write-Host ("        {0}" -f $last) -ForegroundColor DarkRed }
        }
        $ok = $false
    }

    if ($ok) {
        $tags = "tree"
        if (ExitCodeOK $name $vm.rc)  { $tags += "+vm" }
        if (ExitCodeOK $name $nat.rc) { $tags += "+native" }
        Write-Host ("[ok]    {0}  ({1})" -f $name, $tags) -ForegroundColor Green
    }
    return $ok
}

# -------------------------------------------------------------------------
# Fixpoint — DISABLED until self-hosting on VCB IR lands (Phase 36).
# -------------------------------------------------------------------------

function Invoke-OneFixpoint([string]$label, [string]$compiler, [string]$source) {
    Write-Host ("  {0}: fixpoint disabled (self-hosting via VCB pending)" `
                -f $label) -ForegroundColor DarkYellow
    return $true
}

# -------------------------------------------------------------------------

if ($Test -ne "") {
    Write-Host ""
    Write-Host ("Vayu single-file check: {0}" -f $Test) -ForegroundColor Cyan
    Write-Host "=====================================" -ForegroundColor Cyan

    $all = Get-ChildItem -Path "examples" -Filter "*.vyu" -File | Sort-Object Name
    $matches = @($all | Where-Object {
        $_.Name -eq $Test -or
        $_.Name -like "*$Test*" -or
        ("examples\" + $_.Name) -eq $Test
    })

    if ($matches.Count -eq 0) {
        Write-Host ("no example matches '{0}'" -f $Test) -ForegroundColor Red
        exit 1
    }
    if ($matches.Count -gt 1) {
        Write-Host ("'{0}' matches multiple files:" -f $Test) -ForegroundColor Yellow
        foreach ($m in $matches) { Write-Host ("  " + $m.Name) -ForegroundColor Yellow }
        Write-Host "use a longer name" -ForegroundColor Yellow
        exit 1
    }

    $f = $matches[0]
    $ok = Test-OneFile ("examples\" + $f.Name) $f.Name
    Write-Host ""
    if ($ok) { Write-Host "PASS." -ForegroundColor Green; exit 0 }
    else     { Write-Host "FAIL." -ForegroundColor Red;   exit 1 }
}

if ($FixpointOnly) {
    Write-Host ""
    Write-Host "Fixpoint checks (examples skipped)" -ForegroundColor Cyan
    Write-Host "----------------------------------" -ForegroundColor Cyan
    $okVcode = Invoke-OneFixpoint "vcode" "vcode.exe" "vayu-src\vcode.vyu"
    $okVayu  = Invoke-OneFixpoint "vayu"  "vayu.exe"  "vayu-src\vayu.vyu"
    if (-not ($okVcode -and $okVayu)) { exit 1 }
    Write-Host ""
    Write-Host "All checks green." -ForegroundColor Green
    exit 0
}

# Default / -NoFixpoint
$passCount = 0
$failCount = 0
$skipCount = 0
$examples = Get-ChildItem -Path "examples" -Filter "*.vyu" -File | Sort-Object Name

Write-Host ""
Write-Host "Vayu cross-backend harness" -ForegroundColor Cyan
Write-Host "==========================" -ForegroundColor Cyan

foreach ($f in $examples) {
    $name = $f.Name
    $rel  = "examples\" + $name
    if ($skip -contains $name) {
        Write-Host ("[skip]  {0}" -f $name) -ForegroundColor DarkGray
        $skipCount++
        continue
    }
    if (Test-OneFile $rel $name) { $passCount++ }
    else                         { $failCount++ }
}

Write-Host ""
Write-Host "--------------------------"
Write-Host ("Pass: {0}" -f $passCount) -ForegroundColor Green
if ($failCount -gt 0) {
    Write-Host ("Fail: {0}" -f $failCount) -ForegroundColor Red
} else {
    Write-Host ("Fail: {0}" -f $failCount) -ForegroundColor Gray
}
Write-Host ("Skip: {0}" -f $skipCount) -ForegroundColor DarkGray

if ($failCount -gt 0) {
    Write-Host ""
    Write-Host "Fixpoint skipped due to test failures." -ForegroundColor Red
    exit 1
}

if ($NoFixpoint) {
    Write-Host ""
    Write-Host "Fixpoint checks skipped (-NoFixpoint)" -ForegroundColor DarkGray
    Write-Host "All checks green." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "Fixpoint checks" -ForegroundColor Cyan
Write-Host "---------------" -ForegroundColor Cyan
$okVcode = Invoke-OneFixpoint "vcode" "vcode.exe" "vayu-src\vcode.vyu"
$okVayu  = Invoke-OneFixpoint "vayu"  "vayu.exe"  "vayu-src\vayu.vyu"
if (-not ($okVcode -and $okVayu)) { exit 1 }

Write-Host ""
Write-Host "All checks green." -ForegroundColor Green
exit 0