param(
    [string]$BuildDir = "build-release",
    [string]$Configuration = "Release",
    [Alias("MallocIterations")]
    [int]$SmallIters = 24000,
    [int]$ThreadIters = 7000,
    [int]$LargeIters = 900,
    [Alias("VmIterations")]
    [int]$MmapIters = 500,
    [Alias("ProtectIterations")]
    [int]$FixedIters = 250,
    [int]$ReleaseIters = 9000,
    [Alias("MremapIterations")]
    [int]$IgnoredMremapIterations = 0,
    [int]$ArenaMiB = 96,
    [switch]$Stress,
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([System.IO.Path]::IsPathRooted($BuildDir)) {
    $BuildPath = $BuildDir
} else {
    $BuildPath = Join-Path $RepoRoot $BuildDir
}

if (-not $SkipBuild) {
    cmake -S $RepoRoot -B $BuildPath `
        -DUHMAS_BUILD_PROFILE=release `
        -DUHMAS_BUILD_BENCHMARKS=ON
    cmake --build $BuildPath --config $Configuration --parallel
}

$Candidates = @(
    (Join-Path $BuildPath "$Configuration\uhmas_hl_benchmarks.exe"),
    (Join-Path $BuildPath "uhmas_hl_benchmarks.exe")
)
$Exe = $Candidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Exe) {
    throw "uhmas_hl_benchmarks executable not found in $BuildPath"
}

if ($Stress) {
    if (-not $PSBoundParameters.ContainsKey("ArenaMiB")) {
        $ArenaMiB = 256
    }
    if (-not $PSBoundParameters.ContainsKey("SmallIters")) {
        $SmallIters = 240000
    }
    if (-not $PSBoundParameters.ContainsKey("ThreadIters")) {
        $ThreadIters = 70000
    }
    if (-not $PSBoundParameters.ContainsKey("LargeIters")) {
        $LargeIters = 9000
    }
    if (-not $PSBoundParameters.ContainsKey("MmapIters")) {
        $MmapIters = 5000
    }
    if (-not $PSBoundParameters.ContainsKey("FixedIters")) {
        $FixedIters = 2500
    }
    if (-not $PSBoundParameters.ContainsKey("ReleaseIters")) {
        $ReleaseIters = 90000
    }
}

$BenchmarkArgs = @()
if ($Stress) {
    $BenchmarkArgs += "--stress"
}
$BenchmarkArgs += @(
    "--arena-mib", $ArenaMiB,
    "--small-iters", $SmallIters,
    "--thread-iters", $ThreadIters,
    "--large-iters", $LargeIters,
    "--mmap-iters", $MmapIters,
    "--fixed-iters", $FixedIters,
    "--release-iters", $ReleaseIters
)

& $Exe @BenchmarkArgs
