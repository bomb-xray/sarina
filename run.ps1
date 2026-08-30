# smile local CPU/CUDA runner for Windows
# CPU app : powershell -ExecutionPolicy Bypass -File .\run.ps1
# GPU test: powershell -ExecutionPolicy Bypass -File .\run.ps1 -Gpu
param(
    [switch]$Cpu,
    [switch]$Gpu,
    [ValidateRange(10,70)][int]$GpuLimit = 70,
    [ValidateRange(0,86400)][int]$Seconds = 120,
    [ValidateRange(0,255)][int]$Device = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try {
    [Console]::OutputEncoding = [Text.Encoding]::UTF8
    $OutputEncoding = [Text.Encoding]::UTF8
    chcp 65001 > $null
} catch { }

$Dir = if ($PSScriptRoot) { $PSScriptRoot } else { (Get-Location).Path }
Set-Location $Dir
$Port = 8420
if ($Cpu -and $Gpu) { throw 'Choose either -Cpu or -Gpu, not both.' }

function Info($s) { Write-Host "  $s" -ForegroundColor Cyan }
function Good($s) { Write-Host "  $s" -ForegroundColor Green }
function Warn($s) { Write-Host "  $s" -ForegroundColor Yellow }
function Fail($s) { Write-Host "  [!] $s" -ForegroundColor Red }

Write-Host ''
Write-Host '  ===============================================' -ForegroundColor Cyan
Write-Host '    smile - local compute test' -ForegroundColor Cyan
Write-Host '  ===============================================' -ForegroundColor Cyan
Write-Host "  folder: $Dir"
Write-Host ''

# Preserve a running CPU checkpoint before rebuilding.
$oldCpu = Get-Process smile -ErrorAction SilentlyContinue
if ($oldCpu) {
    try {
        Invoke-WebRequest "http://localhost:$Port/shutdown" -UseBasicParsing -TimeoutSec 5 | Out-Null
        Start-Sleep -Milliseconds 700
        Good 'brain.dat saved'
    } catch { Warn 'CPU process did not answer; using its last checkpoint.' }
    $oldCpu | Stop-Process -Force -ErrorAction SilentlyContinue
}
Get-Process smile-gpu -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue

function Find-Tool($name, $candidates) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($p in $candidates) { if ($p -and (Test-Path $p)) { return $p } }
    return $null
}

$nvsmi = Find-Tool 'nvidia-smi' @("$env:ProgramFiles\NVIDIA Corporation\NVSMI\nvidia-smi.exe", "$env:WINDIR\System32\nvidia-smi.exe")
$cudaCandidates = @()
if ($env:CUDA_PATH) { $cudaCandidates += (Join-Path $env:CUDA_PATH 'bin\nvcc.exe') }
$cudaRoot = "$env:ProgramFiles\NVIDIA GPU Computing Toolkit\CUDA"
if (Test-Path $cudaRoot) {
    $cudaCandidates += Get-ChildItem $cudaRoot -Directory -ErrorAction SilentlyContinue |
        Sort-Object Name -Descending | ForEach-Object { Join-Path $_.FullName 'bin\nvcc.exe' }
}
$nvcc = Find-Tool 'nvcc' $cudaCandidates

function Import-VsEnvironment {
    if (Get-Command cl.exe -ErrorAction SilentlyContinue) { return $true }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $vswhere)) { return $false }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { return $false }
    $vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path $vcvars)) { return $false }
    cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
        if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($matches[1])" -Value $matches[2] }
    }
    return [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
}

$hasNvidia = $false
$gpuName = ''
$compute = ''
if ($nvsmi) {
    try {
        $gpuName = (& $nvsmi "--id=$Device" '--query-gpu=name' '--format=csv,noheader,nounits' | Select-Object -First 1).Trim()
        $compute = (& $nvsmi "--id=$Device" '--query-gpu=compute_cap' '--format=csv,noheader,nounits' | Select-Object -First 1).Trim()
        $hasNvidia = [bool]$gpuName
    } catch {
        try { $gpuName = (& $nvsmi "--id=$Device" '--query-gpu=name' '--format=csv,noheader' | Select-Object -First 1).Trim(); $hasNvidia=[bool]$gpuName } catch { }
    }
}

if ($Gpu -and -not $hasNvidia) {
    Fail 'No NVIDIA CUDA device was detected. Run without -Gpu for the CPU application.'
    exit 3
}

if ($Gpu -and $hasNvidia) {
    Info "NVIDIA GPU: $gpuName"
    if ($compute) { Info "compute capability: $compute" }
    if (-not $nvcc) {
        Fail 'CUDA Toolkit compiler (nvcc) was not found.'
        Write-Host ''
        Write-Host '  Install requirements, restart PowerShell, then run this script again:' -ForegroundColor Yellow
        Write-Host '  1) Visual Studio Build Tools 2022 -> Desktop development with C++'
        Write-Host '  2) CUDA Toolkit'
        Write-Host '     GTX 900/10 series: use CUDA 12.9 (CUDA 13 cannot compile for pre-Turing GPUs).'
        Write-Host '     GTX 16 series: CUDA 12.9 or 13.x.'
        Write-Host ''
        Write-Host '  CPU fallback now:  .\run.ps1 -Cpu' -ForegroundColor Cyan
        exit 3
    }
    if (-not (Import-VsEnvironment)) {
        Fail 'MSVC cl.exe was not found; nvcc on Windows needs Visual Studio C++ Build Tools.'
        Write-Host '  Install: winget install Microsoft.VisualStudio.2022.BuildTools'
        Write-Host '  In Visual Studio Installer select: Desktop development with C++'
        exit 4
    }

    $nvccText = (& $nvcc --version 2>&1 | Out-String)
    $cudaMajor = 0
    if ($nvccText -match 'release\s+(\d+)\.') { $cudaMajor = [int]$matches[1] }
    if ($compute -match '^(\d+)\.(\d+)$') {
        $ccNumber = [int]$matches[1] * 10 + [int]$matches[2]
        if ($ccNumber -lt 75 -and $cudaMajor -ge 13) {
            Fail "CUDA $cudaMajor cannot compile for compute $compute. Install CUDA 12.9 for this GTX."
            exit 5
        }
        $arch = "sm_$ccNumber"
    } else {
        $arch = 'native'
        Warn 'compute capability query unavailable; nvcc will use -arch=native.'
    }

    if (-not (Test-Path '.\smile_cuda.cu')) { Fail 'smile_cuda.cu is missing.'; exit 2 }
    $gpuExe = Join-Path $Dir 'smile-gpu.exe'
    Remove-Item $gpuExe -Force -ErrorAction SilentlyContinue
    $log = Join-Path $env:TEMP 'smile_cuda_build.txt'
    Info "building CUDA core ($arch)..."
    & $nvcc '-O3' '-std=c++17' "-arch=$arch" '.\smile_cuda.cu' '-o' $gpuExe 2>&1 |
        Tee-Object -FilePath $log | Out-Null
    if (-not (Test-Path $gpuExe)) {
        Fail 'CUDA build failed:'
        Get-Content $log -Tail 40 | ForEach-Object { Write-Host "  $_" -ForegroundColor DarkRed }
        Write-Host "  full log: $log"
        exit 6
    }
    Good "built: $gpuExe"
    Info "running 32,000 real neurons; GPU utilization ceiling = $GpuLimit%; duration = $Seconds s"
    Write-Host "  A value below $($GpuLimit)% is valid: fixed 32k may still be too small for the GPU." -ForegroundColor Yellow
    Write-Host ''
    & $gpuExe --neurons 32000 --gpu-limit $GpuLimit --seconds $Seconds --device $Device
    exit $LASTEXITCODE
}

# Portable CPU fallback with the complete dashboard/teacher/checkpoint path.
if (-not (Test-Path '.\smile.cpp')) { Fail 'smile.cpp is missing.'; exit 2 }
if (-not (Test-Path '.\persian_words.tsv')) { Fail 'persian_words.tsv is missing.'; exit 2 }
if (-not (Test-Path '.\my_words.tsv')) {
    @('# personal words: word<TAB>frequency<TAB>status<TAB>note') | Set-Content '.\my_words.tsv' -Encoding UTF8
}
$gpp = Find-Tool 'g++' @('C:\msys64\ucrt64\bin\g++.exe','C:\msys64\mingw64\bin\g++.exe','C:\mingw64\bin\g++.exe')
$cpuExe = Join-Path $Dir 'smile.exe'
if ($gpp) {
    Remove-Item $cpuExe -Force -ErrorAction SilentlyContinue
    Info 'building portable CPU application...'
    & $gpp '-O2' '-std=c++17' '-pthread' '.\smile.cpp' '-o' $cpuExe '-lws2_32' '-static'
    if (-not (Test-Path $cpuExe)) { Fail 'CPU build failed.'; exit 8 }
} elseif (Test-Path $cpuExe) {
    Warn 'g++ was not found; using the bundled prebuilt CPU executable.'
} else {
    Fail 'Neither g++ nor the bundled smile.exe was found.'
    Write-Host '  Download the complete CPU package again, or install MSYS2/UCRT64.'
    exit 7
}
# A single content marker supersedes legacy per-size markers. On a size change,
# preserve the old checkpoint and force a clean brain with the requested size.
$requestedSize = '32000'
$sizeMarker = Join-Path $Dir '.brain-size-current'
$currentSize = ''
if (Test-Path $sizeMarker) {
    $currentSize = [string](Get-Content $sizeMarker -Raw -ErrorAction SilentlyContinue)
    $currentSize = $currentSize.Trim()
}
if ($currentSize -ne $requestedSize) {
    if (Test-Path '.\brain.dat') {
        $backup = Join-Path $Dir ("brain-before-32k-{0}.dat" -f (Get-Date -Format 'yyyyMMdd-HHmmss'))
        Copy-Item '.\brain.dat' $backup -Force
        Remove-Item '.\brain.dat' -Force
        Warn "old checkpoint archived: $backup"
    }
    $requestedSize | Set-Content $sizeMarker -Encoding ASCII
}
$cpuArgs = "--neurons $requestedSize --port 8420 --words persian_words.tsv --user-words my_words.tsv"
if (Test-Path '.\brain.dat') { $cpuArgs += ' --load brain.dat'; Good 'continuing 32k brain.dat' }
$proc = Start-Process $cpuExe -ArgumentList $cpuArgs -WorkingDirectory $Dir -PassThru
Info 'building/loading 32k brain; waiting for dashboard...'
$ready = $false
foreach ($i in 1..120) {
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) { Fail "CPU process exited with code $($proc.ExitCode)."; exit 9 }
    try {
        $r = Invoke-WebRequest 'http://localhost:8420/stats' -UseBasicParsing -TimeoutSec 2
        if ($r.StatusCode -eq 200) { $ready = $true; break }
    } catch { }
}
if ($ready) {
    Start-Process 'http://localhost:8420'
    Good 'CPU dashboard: http://localhost:8420'
} else {
    Warn 'brain is still starting; open http://localhost:8420 manually in a moment.'
}
