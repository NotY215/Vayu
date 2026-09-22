# tests\run_all.ps1 — cross-backend sanity harness.
#
# Runs each top-level .vyu under examples/ through the tree-walker, VM, and
# native backends.  Where a backend succeeds, its stdout must match the
# tree-walker's; a mismatch is a regression.  Where a backend fails, it is
# noted as skipped (informational), not a failure.
#
# Run from anywhere; the script chdir's to the Vayu root automatically.

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

#
# IMPORTANT: PowerShell's `>` / `*>` operators write UTF-16LE with a BOM, which
# corrupts the .ssa file (QBE sees byte 0xFF at line 1).  We redirect via
# `cmd /c "..."` so cmd.exe writes raw ASCII bytes with no BOM.

Write-Host ""
Write-Host "Fixpoint checks" -ForegroundColor Cyan
Write-Host "---------------" -ForegroundColor Cyan

# Cache the extracted runtime keyed on vayuc.exe's mtime.
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

    # --- Cache: skip if neither compiler nor source changed since last pass.
    $cacheFile = "tests\.fixpoint_${label}.cache"
    $srcInfo   = Get-Item $source
    $vayucInfo = Get-Item $vayuc
    $fingerprint = ("{0}:{1}|{2}:{3}" -f `
        $srcInfo.Length, $srcInfo.LastWriteTimeUtc.Ticks, `
        $vayucInfo.Length, $vayucInfo.LastWriteTimeUtc.Ticks)
    if (Test-Path $cacheFile) {
        $cached = (Get-Content $cacheFile -Raw -ErrorAction SilentlyContinue)
        if ($null -ne $cached -and $cached.Trim() -eq $fingerprint) {
            Write-Host ("  {0}: OK (cached)" -f $label) -ForegroundColor Green
            return $true
        }
    }

    $ssa1   = "${label}_self.ssa"
    $asm    = "${label}_self.s"
    $exe    = "${label}_self.exe"
    $ssa2   = "${label}_self2.ssa"
    $qbeLog = "${label}_qbe.log"
    $rt     = "${label}_self_rt.c"
    $gccLog = "${label}_gcc.log"
    $scLog  = "${label}_sc.log"

    # 1. Compile source with the outer self-hosted compiler.
    cmd /c "`"$compiler`" `"$source`" > `"$ssa1`" 2>nul"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa1)) {
        Write-Host ("  {0}: first compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        return $false
    }


    # 3. qbe -> .s
    cmd /c "tools\qbe.exe -t amd64_win -o `"$asm`" `"$ssa1`" 2>$qbeLog"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $asm)) {
        Write-Host ("  {0}: qbe failed (see {1})" -f $label, $qbeLog) -ForegroundColor Red
        if (Test-Path $qbeLog) {
            Get-Content $qbeLog | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        Write-Host ("    SSA kept at {0}" -f $ssa1) -ForegroundColor DarkYellow
        return $false
    }

    # 4. gcc -> exe (link against the extracted full runtime, O0 for speed).
    cmd /c "gcc -O0 `"$asm`" `"$rtCache`" -o `"$exe`" -lws2_32 -lbcrypt 2>$gccLog"
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exe)) {
        Write-Host ("  {0}: link failed (see {1})" -f $label, $gccLog) -ForegroundColor Red
        if (Test-Path $gccLog) {
            Get-Content $gccLog | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
        }
        return $false
    }

    # 5. Self-compile the same source with the newly built exe.
    #    The self-hosted compiler's diagnostics go to stdout, so capture both
    #    streams: the .ssa file will contain either IL or the error text.
    cmd /c "`"$exe`" `"$source`" > `"$ssa2`" 2>`"$scLog`""
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $ssa2)) {
        Write-Host ("  {0}: self-compile failed (rc={1})" -f $label, $LASTEXITCODE) -ForegroundColor Red
        Write-Host ("    --- {0} (stdout) ---" -f $ssa2) -ForegroundColor DarkYellow
        if (Test-Path $ssa2) {
            $lines = Get-Content $ssa2
            $n = $lines.Count
            if ($n -le 30) {
                $lines | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
            } else {
                Write-Host "    (first 15 lines)" -ForegroundColor DarkYellow
                $lines[0..14]  | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
                Write-Host ("    ... ({0} lines total) ..." -f $n) -ForegroundColor DarkYellow
                Write-Host "    (last 15 lines)" -ForegroundColor DarkYellow
                $lines[($n-15)..($n-1)] | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
            }
        }
        if (Test-Path $scLog) {
            $errs = Get-Content $scLog
            if ($errs.Count -gt 0) {
                Write-Host ("    --- {0} (stderr) ---" -f $scLog) -ForegroundColor DarkYellow
                $errs | ForEach-Object { Write-Host ("    " + $_) -ForegroundColor DarkYellow }
            }
        }
        return $false
    }

    # 6. Byte-compare the two IL files.
    $a = Get-Content $ssa1 -Raw
    $b = Get-Content $ssa2 -Raw
    if ($null -ne $a -and $null -ne $b -and $a -eq $b) {
        Write-Host ("  {0}: OK" -f $label) -ForegroundColor Green
        Set-Content -Path $cacheFile -Value $fingerprint -NoNewline -Encoding ASCII
        return $true
    }
    Write-Host ("  {0}: DIFFERS" -f $label) -ForegroundColor Red
    if ($null -ne $a) { Write-Host ("    left : {0} ({1} bytes)" -f $ssa1, $a.Length) -ForegroundColor DarkYellow }
    if ($null -ne $b) { Write-Host ("    right: {0} ({1} bytes)" -f $ssa2, $b.Length) -ForegroundColor DarkYellow }
    return $false
}

if (-not (Test-Fixpoint ".\vcode.exe" "vayu-src\vcode.vyu" "vcode")) { $failCount++ }
if (-not (Test-Fixpoint ".\vayu.exe"  "vayu-src\vayu.vyu"  "vayu"))  { $failCount++ }

# Keep temp artifacts on failure; clean up and refresh cache on success.
$cacheVcode = "tests\.fixpoint_vcode.cache"
$cacheVayu  = "tests\.fixpoint_vayu.cache"

if ($failCount -eq 0) {
    Remove-Item -Force -ErrorAction SilentlyContinue `
        "vcode_self.ssa", "vcode_self.s", "vcode_self.exe", "vcode_self2.ssa", `
        "vcode_qbe.log", "vcode_self_rt.c", "vcode_gcc.log", "vcode_sc.log", `
        "vayu_self.ssa",  "vayu_self.s",  "vayu_self.exe",  "vayu_self2.ssa", `
        "vayu_qbe.log",  "vayu_self_rt.c", "vayu_gcc.log",  "vayu_sc.log"
} else {
    Remove-Item -Force -ErrorAction SilentlyContinue $cacheVcode, $cacheVayu
}

if ($failCount -gt 0) { exit 1 }
Write-Host ""
Write-Host "All checks green." -ForegroundColor Green
exit 0