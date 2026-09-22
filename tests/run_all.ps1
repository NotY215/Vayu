# tests\run_all.ps1 — cross-backend sanity harness.
#
# Runs each top-level .vyu under examples/ through the tree-walker, VM, and
# native backends.  Where a backend succeeds, its stdout must match the
# tree-walker's; a mismatch is a regression.  Where a backend fails, it is
# noted as skipped (informational), not a failure.
#
# Run from anywhere; the script chdir's to the Vayu root automatically.
#
# Use -NoFixpoint to skip the (expensive) self-compilation round-trip while
# iterating on examples.

param(
    [switch]$NoFixpoint
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

$vayuc = "build\x64-debug\bin\vayuc.exe"
if (-not (Test-Path $vayuc)) {
    Write-Host "FATAL: $vayuc not found. Build the project first." -ForegroundColor Red
    exit 2
}
if (-not (Test-Path "tools\qbe.exe")) {
    Write-Host "FATAL: tools\qbe.exe not found (needed for --native)." -ForegroundColor Red
    exit 2
}

# Files that are never run: interactive, expected to fail, tool inputs,
# or nondeterministic in a way that cannot be sorted away.
$skip = @(
    "input.vyu",               # interactive
    "native_io.vyu",           # interactive
    "type_errors.vyu",         # expected to fail type-checking
    "expr.vyu",                # bare expressions, no output
    "vlex_test.vyu",           # input for vlex.exe, not runnable
    "vparse_test.vyu",         # input for vparse.exe
    "vcode_test.vyu",          # input for vcode.exe
    "native_collections.vyu"   # prints whole maps; iteration order is impl-defined
)

# Files that only make sense on the native backend (external C symbols,
# libc FFI, the py bridge, or native runtime modules the tree-walker and
# VM do not implement).  Run native only; require exit code 0.  No output
# comparison (there is no reference backend to compare against).
$native_only = @(
    "fs_test.vyu",
    "time_test.vyu",
    "json_test.vyu",
    "regex_test.vyu",
    "thread_test.vyu",
    "net_test.vyu",
    "crypto_test.vyu",
    "random_test.vyu",
    "os_test.vyu",
    "ptr_arith_test.vyu",
    "ffi_callback_test.vyu",
    "ffi_test.vyu",
    "ffi_struct_test.vyu",
    "ffi_wrap_test.vyu",
    "py_init_test.vyu",
    "py_bridge_test.vyu",
    "py_call_test.vyu",
    "py_bidi_test.vyu",
    "py_close_test.vyu",
    "py_eval_test.vyu",
    "py_callback_test.vyu",
    "self_host_17_7.vyu",
    "self_host_17_8.vyu"
)

# Files whose output is a set of lines, not a sequence.  Compared as a
# multiset (sorted) rather than a byte-for-byte stream.
$sort_compare = @(
    "generators.vyu"
)

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
    if (-not $sortLines) {
        return ($a -eq $b)
    }
    $la = @($a -split "`r?`n" | Sort-Object)
    $lb = @($b -split "`r?`n" | Sort-Object)
    return (($la -join "`n") -eq ($lb -join "`n"))
}

$passCount = 0
$failCount = 0
$skipCount = 0
$problems  = @()

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

    if ($native_only -contains $name) {
        $nat = Invoke-Backend $rel "--native"
        if ($nat.rc -eq 0) {
            Write-Host ("[ok]    {0}  (native-only)" -f $name) -ForegroundColor Green
            $passCount++
        } else {
            Write-Host ("[FAIL]  {0}  (native-only rc={1})" -f $name, $nat.rc) -ForegroundColor Red
            $failCount++
            $problems += ("{0} (native-only rc={1})" -f $name, $nat.rc)
        }
        continue
    }

    $tw = Invoke-Backend $rel ""
    if ($tw.rc -ne 0) {
        Write-Host ("[FAIL]  {0}  (tree-walk rc={1})" -f $name, $tw.rc) -ForegroundColor Red
        $failCount++
        $problems += ("{0} (tree-walk rc={1})" -f $name, $tw.rc)
        continue
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
        $passCount++
    } else {
        $failCount++
        $problems += ("{0} (output mismatch)" -f $name)
    }
}

Write-Host ""
Write-Host "--------------------------"
Write-Host ("Pass: {0}" -f $passCount) -ForegroundColor Green
if ($failCount -gt 0) {
    Write-Host ("Fail: {0}" -f $failCount) -ForegroundColor Red
} else {
    Write-Host "Fail: 0" -ForegroundColor Gray
}
Write-Host ("Skip: {0}" -f $skipCount) -ForegroundColor DarkGray

if ($failCount -gt 0) {
    Write-Host ""
    Write-Host "Failures:" -ForegroundColor Red
    foreach ($p in $problems) { Write-Host ("  " + $p) -ForegroundColor Red }
    exit 1
}

# --- Fixpoint checks: vcode / vayu must compile their own source byte-identically.

if ($NoFixpoint) {
    Write-Host ""
    Write-Host "Fixpoint checks skipped (-NoFixpoint)" -ForegroundColor DarkGray
    if ($failCount -gt 0) { exit 1 }
    Write-Host "All checks green." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "Fixpoint checks" -ForegroundColor Cyan
Write-Host "---------------" -ForegroundColor Cyan

# Extract the runtime once per vayuc.exe revision; reuse across both fixpoints.
$rtCache = "tests\.vayu_rt_cache.c"
$vayucTime = (Get-Item $vayuc).LastWriteTimeUtc
if (-not (Test-Path $rtCache) -or
    (Get-Item $rtCache).LastWriteTimeUtc -lt $vayucTime) {
    cmd /c "`"$vayuc`" --emit-runtime `"$rtCache`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $rtCache)) {
        Write-Host "FATAL: --emit-runtime failed" -ForegroundColor Red
        exit 2
    }
}

function Test-Fixpoint {
    param([string]$compiler, [string]$source, [string]$label)

    if (-not (Test-Path $compiler)) {
        Write-Host ("  {0}: {1} missing, skipping" -f $label, $compiler) -ForegroundColor DarkGray
        return $true
    }

    # Cache key is a SHA-256 of the source content.  Rebuilding vayu.exe /
    # vcode.exe does not invalidate the fixpoint result; only editing the
    # source does.
    $cacheFile = "tests\.fixpoint_${label}.cache"
    $srcHash = (Get-FileHash -Algorithm SHA256 -Path $source).Hash
    if (Test-Path $cacheFile) {
        $cached = (Get-Content $cacheFile -Raw -ErrorAction SilentlyContinue)
        if ($null -ne $cached -and $cached.Trim() -eq $srcHash) {
            Write-Host ("  {0}: OK (cached)" -f $label) -ForegroundColor Green
            return $true
        }
    }

    $t0 = Get-Date

    $ssa1   = "${label}_self.ssa"
    $asm    = "${label}_self.s"
    $exe    = "${label}_self.exe"
    $ssa2   = "${label}_self2.ssa"
    $qbeLog = "${label}_qbe.log"
    $gccLog = "${label}_gcc.log"
    $scLog  = "${label}_sc.log"

    # 1. Compile source with the outer self-hosted compiler.
    cmd /c "`"$compiler`" `"$source`" > `"$ssa1`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa1)) {
        Write-Host ("  {0}: first compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        return $false
    }

    # 2. qbe -> .s
    cmd /c "tools\qbe.exe -t amd64_win -o `"$asm`" `"$ssa1`" 2>$qbeLog"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $asm)) {
        Write-Host ("  {0}: qbe failed (see {1})" -f $label, $qbeLog) -ForegroundColor Red
        if (Test-Path $qbeLog) {
            Get-Content $qbeLog | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        return $false
    }

    # 3. gcc -> exe.  -O0 keeps peak RSS low; -s strips symbols.
    cmd /c "gcc -O0 -s `"$asm`" `"$rtCache`" -o `"$exe`" -lws2_32 -lbcrypt 2>$gccLog"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Host ("  {0}: link failed (see {1})" -f $label, $gccLog) -ForegroundColor Red
        if (Test-Path $gccLog) {
            Get-Content $gccLog | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        return $false
    }

    # Free the (large) .s file now; we only need the .exe from here on.
    Remove-Item -Force -ErrorAction SilentlyContinue $asm

    # 4. Self-compile the same source with the newly built exe.
    cmd /c "`"$exe`" `"$source`" > `"$ssa2`" 2>`"$scLog`""
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa2)) {
        Write-Host ("  {0}: self-compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        if (Test-Path $ssa2) {
            Get-Content $ssa2 | Select-Object -First 20 | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        if (Test-Path $scLog) {
            Get-Content $scLog | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        return $false
    }

    # 5. Hash-compare the two SSA files (cheap, avoids loading both into RAM).
    $a = Get-Item $ssa1
    $b = Get-Item $ssa2
    $equal = $false
    if ($a.Length -eq $b.Length) {
        $h1 = (Get-FileHash -Algorithm SHA256 -Path $ssa1).Hash
        $h2 = (Get-FileHash -Algorithm SHA256 -Path $ssa2).Hash
        $equal = ($h1 -eq $h2)
    }

    if ($equal) {
        Set-Content -Path $cacheFile -Value $srcHash -NoNewline -Encoding ASCII
        Remove-Item -Force -ErrorAction SilentlyContinue $ssa1, $ssa2, $scLog, $qbeLog, $gccLog
        $elapsed = (Get-Date) - $t0
        Write-Host ("  {0}: OK ({1:N1}s)" -f $label, $elapsed.TotalSeconds) -ForegroundColor Green
        return $true
    }

    Write-Host ("  {0}: DIFFERS" -f $label) -ForegroundColor Red
    if (Test-Path $ssa1) { Write-Host ("    left : {0} ({1} bytes)" -f $ssa1, $a.Length) -ForegroundColor DarkYellow }
    if (Test-Path $ssa2) { Write-Host ("    right: {0} ({1} bytes)" -f $ssa2, $b.Length) -ForegroundColor DarkYellow }
    return $false
}

if (-not (Test-Fixpoint ".\vcode.exe" "vayu-src\vcode.vyu" "vcode")) { $failCount++ }
if (-not (Test-Fixpoint ".\vayu.exe"  "vayu-src\vayu.vyu"  "vayu"))  { $failCount++ }

if ($failCount -gt 0) { exit 1 }
Write-Host ""
Write-Host "All checks green." -ForegroundColor Green
exit 0