# سارینا

یک مغز دیجیتال رویدادمحور. نه یک مدل زبانی، نه یک شبکه‌ی عصبی کلاسیک.

نورون‌ها خودشان تصمیم می‌گیرند کی فایر کنند، انرژی محدودی دارند، گرسنه می‌شوند و می‌میرند.

## اجرا

```bash
g++ -O2 -std=c++17 -pthread sarina.cpp -o sarina
./sarina --neurons 32000 --port 8420
```

سپس داشبورد: <http://localhost:8420>

| گزینه | کار |
|---|---|
| `--neurons N` | تعداد نورون (پیش‌فرض ۵۰۰۰) |
| `--port P` | پورت داشبورد (پیش‌فرض ۸۴۲۰) |
| `--seed S` | بذر تصادف — اجرای یکسان را تکرار می‌کند |
| `--load brain.dat` | ادامه از چک‌پوینت |
| `--headless N` | اجرای بی‌داشبورد به مدت N ثانیه‌ی مجازی |

## وضعیت

**مرحله‌ی صفر تمام شد.** اقتصاد مانا به تعادل زنده می‌رسد: ۵۰۰۰ نورون، صفر مرگ،
نرخ فایر ثابت ۲٫۶۶ هرتز، استخر قفل‌شده روی ۸۰٪، ~۸۱۰ هزار رویداد در ثانیه.

معماری کامل: [`ARCHITECTURE.md`](ARCHITECTURE.md) · نتایج اجرا: بند ۱۶

## اجرا روی ویندوز (یک خط)

```powershell
irm "https://raw.githubusercontent.com/bomb-xray/sarina/arena/01a00c33-sarina/run.ps1?v=$(Get-Random)" | iex
```

اگر MSYS2 نصب دارید ولی در PATH نیست:

```powershell
$env:Path += ';C:\msys64\ucrt64\bin'
irm "https://raw.githubusercontent.com/bomb-xray/sarina/arena/01a00c33-sarina/run.ps1?v=$(Get-Random)" | iex
```

### اجرای دستی

```powershell
cd $env:USERPROFILE\sarina
g++ -O2 -std=c++17 sarina.cpp -o sarina.exe -lws2_32 -static
.\sarina.exe --neurons 32000 --port 8420
```
