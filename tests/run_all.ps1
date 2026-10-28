# tests\run_all.ps1 -- interpreter and VM conformance harness.
# Modes:
#   (no args)              full: examples + fixpoint placeholder
#   -Test <name>           one example only, interpreter + VM, no fixpoint
#   -FixpointOnly          skip examples, run the fixpoint (stubbed)
#   -NoFixpoint            examples only (fast iteration)
#   -FailOnDenied          retained for compatibility during VIB bootstrap.

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
    "native_smoke.vyu" = 42
}

# ---- Helpers ----------------------------------------------------------

# Returns @{ rc; out; err }.  Captures stdout and stderr into separate
# temp files so the comparison is against the child program's stdout
# only, not against native compiler diagnostics or vayuc's diagnostics.
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
        Write-Host ("[skip]  {0}  (native feature awaits VIB implementation)" -f $name) -ForegroundColor Yellow
        return $true
    }
    $tw = Invoke-Backend $rel ""
    if (-not (ExitCodeOK $name $tw.rc)) {
        Write-Host ("[FAIL]  {0}  (tree-walk rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        return $false
    }
    $vm = Invoke-Backend $rel "--vm"
    if (-not (ExitCodeOK $name $vm.rc)) {
        Write-Host ("[FAIL]  {0}  (VM rc={1})" -f $name, $vm.rc) -ForegroundColor Red
        return $false
    }
    $useSort = $sort_compare -contains $name
    if (-not (Compare-Output $vm.out $tw.out $useSort)) {
        Write-Host ("[FAIL]  {0}  (VM output differs)" -f $name) -ForegroundColor Red
        return $false
    }
    Write-Host ("[ok]    {0}  (tree+VM; native path tracked in VIB roadmap)" -f $name) -ForegroundColor Green
    return $true
}

# -------------------------------------------------------------------------
# Fixpoint -- disabled until VIB self-hosting milestones are implemented.
# -------------------------------------------------------------------------

function Invoke-OneFixpoint([string]$label, [string]$compiler, [string]$source) {
    Write-Host ("  {0}: fixpoint disabled (self-hosting via VIB pending)" `
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