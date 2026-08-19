# ============================================================================
#  سارینا — نصب و اجرای خودکار (ویندوز / PowerShell)
#
#  دانلود می‌کند، کامپایل می‌کند، اجرا می‌کند و داشبورد را باز می‌کند.
#
#  اجرا:
#     irm https://raw.githubusercontent.com/bomb-xray/sarina/arena/01a00c33-sarina/run.ps1 | iex
# ============================================================================

$ErrorActionPreference = 'Stop'

$Repo   = 'https://github.com/bomb-xray/sarina.git'
$Branch = 'arena/01a00c33-sarina'
$Dir    = Join-Path $env:USERPROFILE 'sarina'
$Port   = 8420
$Neurons = 5000

Write-Host ''
Write-Host '  ╔═══════════════════════════════════════════════╗' -ForegroundColor Cyan
Write-Host '  ║   سارینا — مغز دیجیتال                        ║' -ForegroundColor Cyan
Write-Host '  ╚═══════════════════════════════════════════════╝' -ForegroundColor Cyan
Write-Host ''

# --- ۱. بررسی کامپایلر --------------------------------------------------------
$gpp = Get-Command g++ -ErrorAction SilentlyContinue
if (-not $gpp) {
    Write-Host '  [!] کامپایلر g++ پیدا نشد. در حال نصب MSYS2 …' -ForegroundColor Yellow
    $winget = Get-Command winget -ErrorAction SilentlyContinue
    if ($winget) {
        winget install -e --id MSYS2.MSYS2 --accept-source-agreements --accept-package-agreements
        $env:Path += ';C:\msys64\ucrt64\bin;C:\msys64\mingw64\bin'
        C:\msys64\usr\bin\bash.exe -lc "pacman -S --noconfirm mingw-w64-ucrt-x86_64-gcc" 2>$null
    } else {
        Write-Host '  winget موجود نیست. لطفاً MinGW-w64 را دستی نصب کنید:' -ForegroundColor Red
        Write-Host '  https://www.msys2.org' -ForegroundColor Red
        exit 1
    }
    $gpp = Get-Command g++ -ErrorAction SilentlyContinue
    if (-not $gpp) {
        Write-Host '  g++ هنوز در PATH نیست. پنجره‌ی PowerShell را ببندید و دوباره امتحان کنید.' -ForegroundColor Red
        exit 1
    }
}
Write-Host "  ✓ کامپایلر: $($gpp.Source)" -ForegroundColor Green

# --- ۲. دریافت کد ------------------------------------------------------------
if (Test-Path (Join-Path $Dir '.git')) {
    Write-Host '  ✓ به‌روزرسانی مخزن …' -ForegroundColor Green
    Push-Location $Dir
    git fetch origin $Branch  --quiet
    git checkout $Branch      --quiet
    git pull origin $Branch   --quiet
    Pop-Location
} else {
    Write-Host '  ✓ دریافت کد …' -ForegroundColor Green
    $git = Get-Command git -ErrorAction SilentlyContinue
    if ($git) {
        git clone --branch $Branch --depth 1 $Repo $Dir --quiet
    } else {
        New-Item -ItemType Directory -Force -Path $Dir | Out-Null
        $raw = "https://raw.githubusercontent.com/bomb-xray/sarina/$Branch/sarina.cpp"
        Invoke-WebRequest -Uri $raw -OutFile (Join-Path $Dir 'sarina.cpp')
    }
}

Set-Location $Dir

# --- ۳. کامپایل ---------------------------------------------------------------
Write-Host '  ✓ در حال کامپایل …' -ForegroundColor Green
$exe = Join-Path $Dir 'sarina.exe'
& g++ -O2 -std=c++17 -pthread sarina.cpp -o $exe -lws2_32
if ($LASTEXITCODE -ne 0) {
    Write-Host '  [!] کامپایل ناموفق بود.' -ForegroundColor Red
    exit 1
}

# --- ۴. اجرا ------------------------------------------------------------------
Write-Host "  ✓ اجرا روی پورت $Port …" -ForegroundColor Green
$proc = Start-Process -FilePath $exe `
        -ArgumentList "--neurons $Neurons --port $Port" `
        -PassThru -WindowStyle Minimized

Start-Sleep -Seconds 3

# --- ۵. باز کردن داشبورد -------------------------------------------------------
$url = "http://localhost:$Port"
Write-Host ''
Write-Host "  داشبورد: $url" -ForegroundColor Cyan
Write-Host "  توقف   : Stop-Process -Id $($proc.Id)" -ForegroundColor DarkGray
Write-Host ''
Start-Process $url
