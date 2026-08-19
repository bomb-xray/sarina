# ============================================================================
#  سارینا — نصب و اجرای خودکار (ویندوز / PowerShell)
#
#  irm https://raw.githubusercontent.com/bomb-xray/sarina/arena/01a00c33-sarina/run.ps1 | iex
#
#  نیازی به دسترسی ادمین ندارد. اگر کامپایلر نبود، یک نسخه‌ی قابل حمل
#  (w64devkit، حدود ۹۰ مگابایت) را در پوشه‌ی خود پروژه می‌گیرد.
# ============================================================================

$ErrorActionPreference = 'Stop'
$ProgressPreference    = 'Continue'

$Branch  = 'arena/01a00c33-sarina'
$RawBase = "https://raw.githubusercontent.com/bomb-xray/sarina/$Branch"
$Dir     = Join-Path $env:USERPROFILE 'sarina'
$Port    = 8420
$Neurons = 5000

function Step($n, $t) { Write-Host "  [$n/5] $t" -ForegroundColor Cyan }
function Ok($t)       { Write-Host "        $t" -ForegroundColor Green }
function Warn($t)     { Write-Host "        $t" -ForegroundColor Yellow }
function Fail($t)     { Write-Host "  [!] $t" -ForegroundColor Red }

Write-Host ''
Write-Host '  ================================================' -ForegroundColor Cyan
Write-Host '    سارینا - مغز دیجیتال' -ForegroundColor Cyan
Write-Host '  ================================================' -ForegroundColor Cyan
Write-Host ''

# --- ۱. پوشه --------------------------------------------------------------
Step 1 'آماده‌سازی پوشه'
New-Item -ItemType Directory -Force -Path $Dir | Out-Null
Set-Location $Dir
Ok $Dir

# --- ۲. دریافت کد ----------------------------------------------------------
Step 2 'دریافت کد (~۶۰ کیلوبایت)'
try {
    Invoke-WebRequest -Uri "$RawBase/sarina.cpp" -OutFile 'sarina.cpp' -UseBasicParsing -TimeoutSec 90
    $kb = [math]::Round((Get-Item 'sarina.cpp').Length / 1KB, 1)
    Ok "sarina.cpp دریافت شد ($kb KB)"
} catch {
    Fail "دانلود ناموفق: $($_.Exception.Message)"
    Write-Host '  اتصال اینترنت یا دسترسی به گیت‌هاب را بررسی کنید.' -ForegroundColor Red
    return
}

# --- ۳. یافتن کامپایلر -----------------------------------------------------
Step 3 'جست‌وجوی کامپایلر ++C'

$gpp = $null
$cmd = Get-Command g++ -ErrorAction SilentlyContinue
if ($cmd) { $gpp = $cmd.Source }

if (-not $gpp) {
    $known = @(
        "$Dir\w64devkit\bin\g++.exe",
        'C:\msys64\ucrt64\bin\g++.exe',
        'C:\msys64\mingw64\bin\g++.exe',
        'C:\mingw64\bin\g++.exe',
        'C:\ProgramData\mingw64\mingw64\bin\g++.exe',
        "$env:LOCALAPPDATA\Programs\mingw64\bin\g++.exe"
    )
    foreach ($p in $known) { if (Test-Path $p) { $gpp = $p; break } }
}

if ($gpp) {
    Ok "پیدا شد: $gpp"
} else {
    Warn 'کامپایلر نصب نیست.'
    Write-Host ''
    Write-Host '        دریافت w64devkit (قابل حمل، ~۹۰ مگابایت، بدون نیاز به ادمین)' -ForegroundColor Yellow
    Write-Host '        این کار بسته به سرعت اینترنت ۱ تا ۵ دقیقه طول می‌کشد.' -ForegroundColor DarkGray
    Write-Host ''
    $ans = Read-Host '        ادامه بدهم? (y/n)'
    if ($ans -notmatch '^[yY]') {
        Write-Host ''
        Write-Host '  لغو شد. برای نصب دستی کامپایلر:' -ForegroundColor Yellow
        Write-Host '     winget install BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor White
        Write-Host '  یا  https://www.msys2.org' -ForegroundColor White
        return
    }

    $ver = '2.8.0'
    $url = "https://github.com/skeeto/w64devkit/releases/download/v$ver/w64devkit-x64-$ver.7z.exe"
    $sfx = Join-Path $Dir 'w64devkit.7z.exe'

    try {
        Write-Host '        در حال دانلود …' -ForegroundColor DarkGray
        $wc = New-Object System.Net.WebClient
        $wc.DownloadFile($url, $sfx)
        Ok ("دانلود شد ({0} MB)" -f [math]::Round((Get-Item $sfx).Length / 1MB, 0))

        Write-Host '        در حال استخراج …' -ForegroundColor DarkGray
        & $sfx -y "-o$Dir" | Out-Null
        Remove-Item $sfx -Force -ErrorAction SilentlyContinue

        $cand = Join-Path $Dir 'w64devkit\bin\g++.exe'
        if (Test-Path $cand) { $gpp = $cand; Ok 'کامپایلر آماده شد' }
        else { Fail 'استخراج ناموفق بود.'; return }
    } catch {
        Fail "دریافت کامپایلر ناموفق: $($_.Exception.Message)"
        Write-Host '  نصب دستی:  winget install BrechtSanders.WinLibs.POSIX.UCRT' -ForegroundColor White
        return
    }
}

# --- ۴. کامپایل -------------------------------------------------------------
Step 4 'کامپایل (۱۰ تا ۳۰ ثانیه)'
$exe = Join-Path $Dir 'sarina.exe'
if (Test-Path $exe) { Remove-Item $exe -Force -ErrorAction SilentlyContinue }

$log = Join-Path $env:TEMP 'sarina_build.txt'
& $gpp -O2 -std=c++17 sarina.cpp -o $exe -lws2_32 -static 2>&1 | Tee-Object -FilePath $log | Out-Null

if (-not (Test-Path $exe)) {
    Fail 'کامپایل ناموفق بود. خروجی کامپایلر:'
    Get-Content $log -Tail 25 | ForEach-Object { Write-Host "        $_" -ForegroundColor DarkRed }
    return
}
Ok ("sarina.exe ساخته شد ({0} KB)" -f [math]::Round((Get-Item $exe).Length / 1KB, 0))

# --- ۵. اجرا ----------------------------------------------------------------
Step 5 "اجرا روی پورت $Port"

Get-Process sarina -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
Start-Sleep -Milliseconds 400

$proc = Start-Process -FilePath $exe `
        -ArgumentList "--neurons $Neurons --port $Port" `
        -PassThru -WorkingDirectory $Dir

# منتظر بالا آمدن سرور
$url   = "http://localhost:$Port"
$ready = $false
foreach ($i in 1..30) {
    Start-Sleep -Milliseconds 500
    if ($proc.HasExited) { Fail 'برنامه بلافاصله بسته شد.'; return }
    try {
        $r = Invoke-WebRequest -Uri "$url/stats" -UseBasicParsing -TimeoutSec 2
        if ($r.StatusCode -eq 200) { $ready = $true; break }
    } catch { }
}

Write-Host ''
if ($ready) {
    Ok 'مغز زنده است.'
    Write-Host ''
    Write-Host "  داشبورد : $url" -ForegroundColor Cyan
    Write-Host "  توقف    : Stop-Process -Id $($proc.Id)" -ForegroundColor DarkGray
    Write-Host ''
    Start-Process $url
} else {
    Warn "سرور پاسخ نداد. شاید پورت $Port اشغال است."
    Write-Host "  اجرای دستی با پورت دیگر:" -ForegroundColor White
    Write-Host "     cd `"$Dir`"; .\sarina.exe --port 9000" -ForegroundColor White
}
