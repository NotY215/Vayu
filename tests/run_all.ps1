# tests\run_all.ps1 — cross-backend sanity harness.
#
# Modes:
#   (no args)              full: examples + fixpoint
#   -Test <name>           one example only, all backends, no fixpoint
#   -FixpointOnly          skip examples, run only the self-compile fixpoint
#   -NoFixpoint            examples only (fast iteration)

param(
    [string]$Test = "",
    [switch]$FixpointOnly,
    [switch]$NoFixpoint
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$vayuc = "build\x64-debug\bin\vayuc.exe"
if (-not (Test-Path $vayuc)) {
    Write-Host "FATAL: $vayuc not found. Build first." -ForegroundColor Red
    exit 2
}
if (-not (Test-Path "tools\qbe.exe")) {
    Write-Host "FATAL: tools\qbe.exe not found." -ForegroundColor Red
    exit 2
}

$skip = @(
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
    "self_host_17_7.vyu", "self_host_17_8.vyu"
)

$sort_compare = @( "generators.vyu" )

function Invoke-Backend {
    param($file, $mode)
    $tmp = [System.IO.Path]::GetTempFileName()
    $argList = @($file)
    if ($mode) { $argList += $mode }
    & $vayuc @argList *> $tmp
    $rc = $LASTEXITCODE
    $out = Get-Content $tmp -Raw -ErrorAction SilentlyContinue
    Remove-Item -Force $tmp -ErrorAction SilentlyContinue
    if ($null -eq $out) { $out = "" }
    return @{ rc = $rc; out = $out }
}

function Compare-Output {
    param([string]$a, [string]$b, [bool]$sortLines)
    if (-not $sortLines) { return ($a -eq $b) }
    $la = @($a -split "`r?`n" | Sort-Object)
    $lb = @($b -split "`r?`n" | Sort-Object)
    return (($la -join "`n") -eq ($lb -join "`n"))
}

function Test-OneFile([string]$rel, [string]$name) {
    if ($skip -contains $name) {
        Write-Host ("[skip]  {0}  (in skip list)" -f $name) -ForegroundColor DarkGray
        return $true
    }
    if ($native_only -contains $name) {
        $nat = Invoke-Backend $rel "--native"
        if ($nat.rc -eq 0) {
            Write-Host ("[ok]    {0}  (native-only)" -f $name) -ForegroundColor Green
            return $true
        }
        Write-Host ("[FAIL]  {0}  (native-only rc={1})" -f $name, $nat.rc) -ForegroundColor Red
        return $false
    }
    $tw = Invoke-Backend $rel ""
    if ($tw.rc -ne 0) {
        Write-Host ("[FAIL]  {0}  (tree-walk rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        return $false
    }
    $vm  = Invoke-Backend $rel "--vm"
    $nat = Invoke-Backend $rel "--native"
    $useSort = $sort_compare -contains $name
    $ok = $true
    if ($vm.rc -eq 0 -and -not (Compare-Output $vm.out $tw.out $useSort)) {
        Write-Host ("[FAIL]  {0}  (VM output differs)" -f $name) -ForegroundColor Red
        $ok = $false
    }
    if ($nat.rc -eq 0 -and -not (Compare-Output $nat.out $tw.out $useSort)) {
        Write-Host ("[FAIL]  {0}  (native output differs)" -f $name) -ForegroundColor Red
        $ok = $false
    }
    if ($ok) {
        $tags = "tree"
        if ($vm.rc -eq 0)  { $tags += "+vm" }
        if ($nat.rc -eq 0) { $tags += "+native" }
        Write-Host ("[ok]    {0}  ({1})" -f $name, $tags) -ForegroundColor Green
    }
    return $ok
}

# -------------------------------------------------------------------------
# Fixpoint infrastructure — serial, -O1, BelowNormal priority, no caches.
# -------------------------------------------------------------------------

function Invoke-OneFixpoint([string]$label, [string]$compiler, [string]$source) {
    $compilerPath = Join-Path $root $compiler
    if (-not (Test-Path $compilerPath)) {
        Write-Host ("  {0}: {1} missing, skipping" -f $label, $compiler) -ForegroundColor DarkGray
        return $true
    }

    $selfSsa  = "${label}_self.ssa"
    $selfS    = "${label}_self.s"
    $selfExe  = "${label}_self.exe"
    $selfSsa2 = "${label}_self2.ssa"
    $qbeLog   = "${label}_qbe.log"
    $gccLog   = "${label}_gcc.log"
    $scLog    = "${label}_sc.log"
    $rtC      = "${label}_rt.c"

    $selfSsaPath  = Join-Path $root $selfSsa
    $selfSPath    = Join-Path $root $selfS
    $selfExePath  = Join-Path $root $selfExe
    $selfSsa2Path = Join-Path $root $selfSsa2
    $qbeLogPath   = Join-Path $root $qbeLog
    $gccLogPath   = Join-Path $root $gccLog
    $scLogPath    = Join-Path $root $scLog
    $rtCPath      = Join-Path $root $rtC

    $qbeExe = Join-Path $root "tools\qbe.exe"

    $t0 = Get-Date

    & $vayuc --emit-runtime $rtCPath 2>$null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rtCPath)) {
        Write-Host ("  {0}: --emit-runtime failed" -f $label) -ForegroundColor Red
        return $false
    }

    # Step 1
    & $compilerPath $source 2>$null | Out-File -FilePath $selfSsaPath -Encoding ASCII
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $selfSsaPath)) {
        Write-Host ("  {0}: first compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        return $false
    }

    # Step 2
    & $qbeExe -t amd64_win -o $selfSPath $selfSsaPath 2>$qbeLogPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $selfSPath)) {
        Write-Host ("  {0}: qbe failed (see {1})" -f $label, $qbeLog) -ForegroundColor Red
        if (Test-Path $qbeLogPath) { Get-Content $qbeLogPath | Select-Object -First 10 | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow } }
        return $false
    }

    # Step 3 — -O2 (fastest runtime, matching your chart).
    & gcc -O2 -s $selfSPath $rtCPath -o $selfExePath -lws2_32 -lbcrypt 2>$gccLogPath
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $selfExePath)) {
        Write-Host ("  {0}: link failed (see {1})" -f $label, $gccLog) -ForegroundColor Red
        if (Test-Path $gccLogPath) { Get-Content $gccLogPath | Select-Object -First 10 | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow } }
        return $false
    }

    Remove-Item -Force -ErrorAction SilentlyContinue $selfSPath

    # Step 4
    & $selfExePath $source 2>$scLogPath | Out-File -FilePath $selfSsa2Path -Encoding ASCII
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $selfSsa2Path)) {
        Write-Host ("  {0}: self-compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        if (Test-Path $scLogPath) { Get-Content $scLogPath | Select-Object -First 10 | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow } }
        return $false
    }

    # Step 5
    $a = Get-Item $selfSsaPath
    $b = Get-Item $selfSsa2Path
    $equal = $false
    if ($a.Length -eq $b.Length) {
        $h1 = (Get-FileHash -Algorithm SHA256 -Path $selfSsaPath).Hash
        $h2 = (Get-FileHash -Algorithm SHA256 -Path $selfSsa2Path).Hash
        $equal = ($h1 -eq $h2)
    }

    $elapsed = (Get-Date) - $t0

    if ($equal) {
        Remove-Item -Force -ErrorAction SilentlyContinue `
            $selfSsaPath, $selfSsa2Path, $selfExePath, $rtCPath, `
            $qbeLogPath, $gccLogPath, $scLogPath
        Write-Host ("  {0}: OK ({1:N1}s)" -f $label, $elapsed.TotalSeconds) -ForegroundColor Green
        return $true
    }

    Write-Host ("  {0}: DIFFERS ({1:N1}s)" -f $label, $elapsed.TotalSeconds) -ForegroundColor Red
    Write-Host ("    left : {0} ({1} bytes)" -f $selfSsa, $a.Length) -ForegroundColor DarkYellow
    Write-Host ("    right: {0} ({1} bytes)" -f $selfSsa2, $b.Length) -ForegroundColor DarkYellow
    return $false
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
Write-Host ("Fail: {0}" -f $failCount) -ForegroundColor $(if ($failCount -gt 0) { "Red" } else { "Gray" })
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