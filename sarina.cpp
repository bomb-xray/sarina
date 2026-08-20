// ============================================================================
//  سارینا — مرحله‌ی صفر
//  Sarina — Stage 0 prototype
//
//  یک مغز دیجیتال رویدادمحور. تک‌فایل، بدون وابستگی.
//  هدف این مرحله: پاسخ به تنها سؤالی که با فکر کردن حل نمی‌شود —
//      «آیا اقتصاد مانا به تعادل زنده می‌رسد یا به انفجار/انجماد می‌افتد؟»
//
//  build:  g++ -O2 -std=c++17 -pthread sarina.cpp -o sarina
//  run:    ./sarina [--neurons N] [--port P] [--seed S] [--load brain.dat]
//
//  مرجع: ARCHITECTURE.md  (پیش‌نویس ۰٫۳)
// ============================================================================

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
  #define MSG_NOSIGNAL 0
  static inline int  close_sock(SOCKET s) { return closesocket(s); }
  using sock_t = SOCKET;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  static inline int  close_sock(int s) { return close(s); }
  using sock_t = int;
  #define INVALID_SOCKET (-1)
#endif

using i32 = int32_t;
using i64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

// ============================================================================
//  ۱. واحدها و ثابت‌ها  —  همه‌ی حساب‌ها ثابت‌ممیز، برای تکرارپذیری
// ============================================================================

using vtime = i64;                    // زمان مجازی، بر حسب میکروثانیه
static constexpr vtime US  = 1;
static constexpr vtime MS  = 1000;
static constexpr vtime SEC = 1000000;

static constexpr i64 MANA = 1000;     // ۱ مانا = ۱۰۰۰ واحد داخلی (میلی‌مانا)

// --- نورون -----------------------------------------------------------------
enum Kind : u8 { K_NORMAL = 0, K_MEMORY = 1, K_GIANT = 2 };
enum Lobe : u8 { L_INPUT = 0, L_CENTRAL = 1, L_OUTPUT = 2, N_LOBES = 3 };
enum State : u8 { S_HEALTHY = 0, S_IGNORE = 1, S_SPAM = 2, S_DORMANT = 3, S_ASLEEP = 4, S_DEAD = 5 };

static const int  KIND_LINES[3] = { 20,   40,    60   };      // بند ۲٫۱
static const int  KIND_MEM[3]   = { 32,   1024,  4096 };
static const i64  KIND_CAP[3]   = { 20,   40,    120  };      // سقف مانا
static const int  KIND_FUEL[3]  = { 64,   512,   8192 };      // بند ۹٫۱
static const vtime KIND_CADENCE[3] = { 10*MS, 15*MS, 50*MS };

// --- اقتصاد (بند ۵) ---------------------------------------------------------
static constexpr i64 FIRE_STARTUP   = 1200;       // ۱٫۲ مانا  — هزینه‌ی راه‌اندازی
static constexpr i64 FIRE_PER_LINE  = 300;        // ۰٫۳ مانا  — هر خط
static constexpr vtime TRANSIT_TIME = SEC / 2;    // بازگشت مانای سوخته
static constexpr i64 BASE_INCOME    = 260;        // زیر هزینه‌ی زنده‌ماندن → کمبود واقعی
static constexpr i64 UPKEEP_PCT     = 20;         // ۲٪ سقف در ثانیه (‰)
static constexpr i64 LEAK_BACK      = 50;         // ۵۰٪ نشت به عقب
static constexpr i64 LEAK_FWD       = 10;         // ۱۰٪ نشت به جلو

// --- مرگ (بند ۶) ------------------------------------------------------------
static constexpr i64 IGNORE_PCT      = 20;        // زیر ۲۰٪ ظرفیت → ایگنور
static constexpr vtime STARVE_TIME   = 10 * SEC;  // بی‌درآمدی تا شروع اسپم
static constexpr vtime SPAM_TIME     = 2 * SEC;
static constexpr i64 DEATH_CREDIT    = 5 * MANA;  // اعتبار پایانی اسپم
static constexpr i64 DEATH_CAP_PPT   = 1;         // سقف نرخ مرگ ۰٫۱٪ (‰) در ثانیه
static constexpr vtime DORMANT_TIME  = 30 * SEC;  // خواب زمستانی

// --- زمان‌بندی --------------------------------------------------------------
static constexpr vtime REFRACTORY = 40 * MS;      // دوره‌ی تعلیق پس از هر فایر (ضد اسپم)
static constexpr vtime SYS_TICK   = 50 * MS;      // سیستم‌تیک: درآمد، مالیات، مرگ
static constexpr vtime EDGE_MIN   = 1 * MS;
static constexpr vtime EDGE_MAX   = 20 * MS;

// ============================================================================
//  ۲. تصادف قابل تکرار  —  هر نورون مولد مستقل خودش را دارد (بند ۲٫۵)
// ============================================================================

struct Rng {
    u64 s;
    explicit Rng(u64 seed = 0x9E3779B97F4A7C15ull) : s(seed ? seed : 1) {}
    inline u64 next() {                       // splitmix64
        u64 z = (s += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    inline u32 u32r()                { return (u32)(next() >> 32); }
    inline u32 below(u32 n)          { return n ? u32r() % n : 0; }
    inline i64 range(i64 a, i64 b)   { return a + (i64)below((u32)(b - a + 1)); }
};

// ============================================================================
//  ۳. ماشین مجازی  (بند ۹٫۱)
//     ۱۶ ثبات ۳۲ بیتی · دستور ۴ بایتی ثابت · بدون پشته
//     [کد عمل: ۸][مقصد: ۴][عملوند: ۴][فوری/عملوند۲: ۱۶]
// ============================================================================

enum Op : u8 {
    OP_NOP = 0,
    OP_IMM, OP_MOV,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_AND, OP_OR,  OP_XOR, OP_NOT, OP_SHL, OP_SHR,
    OP_EQ,  OP_LT,  OP_GT,  OP_SEL,
    OP_LD,  OP_ST,
    OP_SENSE, OP_FIRE, OP_SLEEP,
    OP_LOOP, OP_ENDL,                    // حلقه‌ی باز شده — حافظه‌ای و غول
    OP_JMP, OP_JZ, OP_JNZ,               // پرش — فقط غول
    OP_HALT,
    OP__COUNT
};

// کانال‌های حسگر
enum Sense : u16 {
    SN_INPOP = 0,     // تعداد خطوط ورودی فعال
    SN_INBITS,        // ۱۶ بیت پایین ورودی
    SN_FRESHPOP,      // تعداد خطوط تازه
    SN_MANA,          // مانا (واحد کامل)
    SN_MANAPCT,       // درصد پرشدگی مخزن
    SN_TSF,           // میلی‌ثانیه از آخرین فایر
    SN_TSI,           // میلی‌ثانیه از آخرین ورودی
    SN_NOISE,         // نویز ۰..۲۵۵ (تحت تأثیر دما)
    SN_POOLPCT,       // پرشدگی استخر لوب
    SN_LINE0 = 64     // SN_LINE0 + i  →  بیت خط i
};

static inline u32 enc(u8 op, u8 d, u8 a, u16 imm) {
    return ((u32)op << 24) | ((u32)(d & 15) << 20) | ((u32)(a & 15) << 16) | imm;
}
static inline u16 IM(i32 v) { return (u16)(0x8000 | (v & 0x7FFF)); }   // حالت فوری
static inline u16 RG(int r) { return (u16)(r & 15); }                  // حالت ثباتی

struct Program {
    std::vector<u32> code;
    u32 refs = 0;
};

// مخزن مشترک بایت‌کد (بند ۱۲٫۶) — نورون فقط شناسه نگه می‌دارد
static std::vector<Program> g_progs;

// ============================================================================
//  ۴. نورون
// ============================================================================

struct Edge {
    u32   dst;
    u8    line;
    vtime delay;
};

struct Neuron {
    u32  id     = 0;
    u8   kind   = K_NORMAL;
    u8   lobe   = L_CENTRAL;
    u8   state  = S_HEALTHY;
    u8   half   = 0;                 // نیمه‌ی الف/ب لوب ورودی
    i32  x = 0, y = 0;               // مختصات — برای سیم‌کشی محلی‌گرا (بند ۱۲٫۳)

    i64  mana     = 0;               // میلی‌مانا
    i64  cap      = 0;
    u16  credit   = 0;               // اعتبار — ۲ بایت (بند ۱۶٫۶)؛ ۸ بیتی هم کافی است
    i64  dcredit  = 0;               // اعتبار پایانی برای اسپم

    vtime last_eval  = 0;
    vtime last_fire  = -1;
    vtime last_input = -1;
    vtime last_income = 0;
    vtime spam_until = 0;

    u32  prog = 0;
    Rng  rng{1};

    u64  in_bits = 0;                // تا ۶۰ خط
    std::vector<vtime> in_at;        // زمان رسیدن هر خط (برای بیت تازگی)
    std::vector<Edge>  out;
    std::vector<u8>    mem;

    // آمار
    u32 fires = 0;
    u32 faults = 0;

    inline int lines() const { return KIND_LINES[kind]; }
};

// ============================================================================
//  ۵. رویداد
// ============================================================================

enum EvType : u8 { EV_EVAL = 0, EV_SIGNAL = 1, EV_SYS = 2, EV_TRANSIT = 3 };

struct Event {
    vtime t;
    u32   target;
    u8    type;
    u8    line;
    u8    bit;
    u32   seq;                        // شکستن تساوی — تکرارپذیری
};
struct EvCmp {
    bool operator()(const Event& a, const Event& b) const {
        if (a.t != b.t) return a.t > b.t;
        return a.seq > b.seq;
    }
};

// ============================================================================
//  ۶. مغز
// ============================================================================

struct LobePool {
    i64 pool     = 0;
    i64 target   = 0;                 // ظرفیت هدف
    i64 alive    = 0;
    i64 cap_sum  = 0;                 // مجموع سقف مانای نورون‌های زنده
    i64 deaths_window = 0;
    bool emergency = false;
};

// --- دستگاه: کلمه‌ی خروجی که منتظر نمره است (بند ۱۱٫۲) ---
struct OutWord {
    u32         id;
    std::string text;
    double      t;          // زمان مجازی تولد، ثانیه
    int         score;      // نمره‌ی داده‌شده
    bool        scored;
};

// --- پیام چت ---
struct ChatMsg {
    u32         id;
    bool        from_human;
    std::string text;
    double      t;
};

struct Stats {
    i64 vtime_us = 0;
    double wall_s = 0;
    i64 alive = 0, healthy = 0, ignoring = 0, spamming = 0, dormant = 0, asleep = 0, dead = 0;
    i64 fires = 0, signals = 0, faults = 0;
    double fire_hz = 0;
    i64 pool[N_LOBES]  = {0,0,0};
    i64 ptgt[N_LOBES]  = {1,1,1};
    i64 alive_lobe[N_LOBES] = {0,0,0};
    i64 transit = 0;
    i64 total_mana = 0;
    i64 events = 0;
    double ev_per_s = 0;
    std::string out_text;
    std::vector<double> hist_fire, hist_pool, hist_alive;
    std::vector<OutWord> words;
    std::vector<ChatMsg> chat;
    i64 words_total = 0, words_scored = 0;
    double avg_score = 0;
};

struct Brain {
    std::vector<Neuron> n;
    LobePool            lp[N_LOBES];
    std::priority_queue<Event, std::vector<Event>, EvCmp> q;

    vtime now  = 0;
    u32   seq  = 0;
    u64   seed = 12345;
    Rng   grng{12345};

    // مانای در ترانزیت (بند ۵٫۲)
    std::deque<std::pair<vtime,i64>> transit;
    i64 transit_total = 0;

    i64 treasury = 0;                 // خزانه‌ی سیستم برای درآمد پایه

    // دما (بند ۲٫۵)
    std::atomic<int> temperature{100};   // ۰..۲۵۵

    // شمارنده‌ها
    i64 c_fires = 0, c_signals = 0, c_faults = 0, c_events = 0;
    i64 c_fires_prev = 0; vtime t_prev = 0;

    // خروجی مغز → دستگاه
    std::vector<u8> out_bits;
    std::string     out_text;

    // --- دستگاه: بخش زبان ---
    std::string          cur_word;        // کلمه‌ی در حال ساخت
    std::vector<OutWord> words;           // کلمات کامل‌شده
    std::vector<ChatMsg> chat;
    u32                  next_word_id = 1;
    u32                  next_msg_id  = 1;
    i64                  words_total  = 0;
    i64                  words_scored = 0;
    double               score_sum    = 0;

    // --- دستگاه: بخش حواس (ورودی انسان → مغز) ---
    std::string     utf8_buf;             // بافر دنباله‌ی UTF-8
    int             utf8_need = 0;        // چند بایت ادامه لازم است
    i64             chars_ok = 0, chars_bad = 0;
    std::deque<u8>  in_queue;             // بیت‌های منتظر تزریق
    std::deque<u8>  mirror_queue;         // آینه: خروجی خودش با تأخیر
    vtime           next_inject = 0;

    inline void push(vtime t, u32 target, u8 type, u8 line = 0, u8 bit = 0) {
        q.push(Event{t, target, type, line, bit, seq++});
    }
};

static Brain      B;
static std::mutex g_mx;
static Stats      g_stats;
static std::atomic<bool> g_running{true};
static std::atomic<bool> g_paused{false};
static std::atomic<int>  g_speed{1000};      // ‰ نسبت به بی‌درنگ؛ ۰ = بیشینه
static std::atomic<bool> g_shutdown_req{false};
static std::atomic<i64>  g_reward_pending{0};
static bool              g_open_browser = true;
static std::atomic<bool> g_server_up{false};
static vtime             g_stop_at = 0;      // ۰ = بی‌نهایت

// ============================================================================
//  ۷. برنامه‌های بذر  —  توابع اولیه را ما می‌نویسیم (بند ۹)
// ============================================================================

// نورون عادی: بایت‌کد خطی محض. بدون پرش، بدون حلقه.
// منطق: برانگیختگی از ورودی + تازگی + نویز + بی‌قراری؛ با SEL شرط می‌سازیم.
static std::vector<u32> seed_normal(int thresh, int width, int greed) {
    std::vector<u32> c;
    // --- حسگرها ---
    c.push_back(enc(OP_SENSE, 1, 0, SN_INPOP));       // r1 = تعداد ورودی فعال
    c.push_back(enc(OP_SENSE, 2, 0, SN_FRESHPOP));    // r2 = تعداد تازه
    c.push_back(enc(OP_SENSE, 3, 0, SN_NOISE));       // r3 = نویز ۰..۲۵۵
    c.push_back(enc(OP_SENSE, 4, 0, SN_MANAPCT));     // r4 = ٪ مانا
    c.push_back(enc(OP_SENSE, 5, 0, SN_TSF));         // r5 = ms از آخرین فایر
    c.push_back(enc(OP_SENSE, 6, 0, SN_INBITS));      // r6 = الگوی بیت ورودی

    // --- برانگیختگی: ورودی + تازگی + نویز + بی‌قراری ---
    c.push_back(enc(OP_MUL, 7, 1, IM(24)));
    c.push_back(enc(OP_MUL, 8, 2, IM(40)));
    c.push_back(enc(OP_ADD, 7, 7, RG(8)));
    c.push_back(enc(OP_DIV, 8, 3, IM(6)));
    c.push_back(enc(OP_ADD, 7, 7, RG(8)));
    c.push_back(enc(OP_DIV, 8, 5, IM(40)));
    c.push_back(enc(OP_ADD, 7, 7, RG(8)));            // r7 = برانگیختگی

    // --- تصمیم فایر: برانگیخته و پول‌دار ---
    c.push_back(enc(OP_GT,  9, 7, IM(thresh)));
    c.push_back(enc(OP_GT, 10, 4, IM(greed)));
    c.push_back(enc(OP_AND, 9, 9, RG(10)));           // r9 = فایر کنم؟

    // --- ماسک پایه، پهن‌تر وقتی برانگیختگی بالاست ---
    c.push_back(enc(OP_IMM, 11, 0, IM(width)));
    c.push_back(enc(OP_GT,  12, 7, IM(thresh * 2)));
    c.push_back(enc(OP_SHL, 13, 11, IM(2)));
    c.push_back(enc(OP_OR,  13, 13, RG(11)));
    c.push_back(enc(OP_SEL, 11, 12, (u16)((13 << 4) | 11)));   // r11 = c ? r13 : r11

    // --- چرخش حقیقی روی ۲۰ خط: هر خط شانس برابر، از جمله ۱۸ و ۱۹ ---
    c.push_back(enc(OP_MOD, 14, 3, IM(20)));          // r14 = k = نویز % 20
    c.push_back(enc(OP_SHL, 12, 11, RG(14)));         // r12 = base << k
    c.push_back(enc(OP_IMM, 13, 0, IM(20)));
    c.push_back(enc(OP_SUB, 13, 13, RG(14)));         // r13 = 20-k
    c.push_back(enc(OP_SHR, 13, 11, RG(13)));         // r13 = base >> (20-k)
    c.push_back(enc(OP_OR,  11, 12, RG(13)));         // چرخش کامل
    c.push_back(enc(OP_SHL, 12, 11, IM(6)));          // نسخه‌ی جابه‌جاشده به خطوط بالا
    c.push_back(enc(OP_OR,  11, 11, RG(12)));         // پوشش هر ۲۰ خط
    c.push_back(enc(OP_SEL, 11,  9, (u16)((11 << 4) | 0)));    // نه‌فایر → ۰

    // --- الگوی بیت: در تمام ۲۰ خط پخش می‌شود (اصلاح مرحله‌ی ۰) ---
    // نویز ۸ بیتی است؛ با سه جابه‌جایی، بیت‌های ۰..۲۳ را پر می‌کند
    c.push_back(enc(OP_MOV, 15, 3, RG(0)));
    c.push_back(enc(OP_SHL, 12, 3, IM(7)));
    c.push_back(enc(OP_XOR, 15, 15, RG(12)));
    c.push_back(enc(OP_SHL, 12, 3, IM(13)));
    c.push_back(enc(OP_XOR, 15, 15, RG(12)));
    c.push_back(enc(OP_XOR, 15, 15, RG(6)));
    c.push_back(enc(OP_FIRE, 0, 11, RG(15)));
    c.push_back(enc(OP_HALT, 0, 0, 0));
    return c;
}

// نورون حافظه‌ای: بایت‌کد نیمه‌خطی — حلقه‌ی باز شده مجاز (بند ۱۲٫۲)
// منطق: ورودی را در حافظه انباشت می‌کند و بر اساس تاریخچه تصمیم می‌گیرد.
static std::vector<u32> seed_memory(int thresh) {
    std::vector<u32> c;
    c.push_back(enc(OP_SENSE, 1, 0, SN_INPOP));
    c.push_back(enc(OP_SENSE, 2, 0, SN_FRESHPOP));
    c.push_back(enc(OP_SENSE, 3, 0, SN_NOISE));
    c.push_back(enc(OP_SENSE, 4, 0, SN_MANAPCT));
    c.push_back(enc(OP_SENSE, 6, 0, SN_INBITS));

    // مکان‌نمای حلقوی در حافظه
    c.push_back(enc(OP_LD,  5, 0, IM(0)));            // r5 = cursor
    c.push_back(enc(OP_ADD, 5, 5, IM(1)));
    c.push_back(enc(OP_MOD, 5, 5, IM(15)));
    c.push_back(enc(OP_ST,  5, 0, IM(0)));

    c.push_back(enc(OP_ADD, 7, 5, IM(1)));
    c.push_back(enc(OP_ST,  1, 7, RG(0)));            // history[cursor+1] = inpop

    // جمع تاریخچه با حلقه‌ی باز شده (سقف ۸)
    c.push_back(enc(OP_IMM,  8, 0, IM(0)));           // مجموع
    c.push_back(enc(OP_IMM,  9, 0, IM(1)));           // اندیس
    c.push_back(enc(OP_LOOP, 0, 0, IM(8)));
    c.push_back(enc(OP_LD,  10, 9, RG(0)));
    c.push_back(enc(OP_ADD,  8, 8, RG(10)));
    c.push_back(enc(OP_ADD,  9, 9, IM(1)));
    c.push_back(enc(OP_ENDL, 0, 0, 0));

    // excite = sum*6 + fresh*30 + noise/8
    c.push_back(enc(OP_MUL,  7, 8, IM(6)));
    c.push_back(enc(OP_MUL, 10, 2, IM(30)));
    c.push_back(enc(OP_ADD,  7, 7, RG(10)));
    c.push_back(enc(OP_DIV, 10, 3, IM(8)));
    c.push_back(enc(OP_ADD,  7, 7, RG(10)));

    c.push_back(enc(OP_GT,  11, 7, IM(thresh)));
    c.push_back(enc(OP_GT,  12, 4, IM(20)));
    c.push_back(enc(OP_AND, 11, 11, RG(12)));

    c.push_back(enc(OP_IMM, 13, 0, IM(0x3F)));
    c.push_back(enc(OP_SEL, 13, 11, (u16)((0 << 4) | 13)));
    c.push_back(enc(OP_SENSE, 15, 0, SN_NOISE));
    c.push_back(enc(OP_XOR,   14, 6, RG(8)));
    c.push_back(enc(OP_XOR,   14, 14, RG(15)));
    c.push_back(enc(OP_FIRE,   0, 13, RG(14)));
    c.push_back(enc(OP_HALT, 0, 0, 0));
    return c;
}

// غول‌پیکر: بایت‌کد کامل — پرش مجاز
static std::vector<u32> seed_giant() {
    std::vector<u32> c;
    c.push_back(enc(OP_SENSE, 1, 0, SN_INPOP));
    c.push_back(enc(OP_SENSE, 2, 0, SN_FRESHPOP));
    c.push_back(enc(OP_SENSE, 3, 0, SN_NOISE));
    c.push_back(enc(OP_SENSE, 4, 0, SN_MANAPCT));
    c.push_back(enc(OP_SENSE, 5, 0, SN_POOLPCT));

    c.push_back(enc(OP_LD,   6, 0, IM(0)));           // شمارنده‌ی دراز‌مدت
    c.push_back(enc(OP_ADD,  6, 6, IM(1)));
    c.push_back(enc(OP_ST,   6, 0, IM(0)));

    // اگر استخر خیلی خالی است، محافظه‌کار شو  →  پرش
    c.push_back(enc(OP_GT,   7, 5, IM(25)));
    c.push_back(enc(OP_JZ,   0, 7, IM(20)));          // به «خروج آرام»

    c.push_back(enc(OP_MUL,  8, 1, IM(20)));
    c.push_back(enc(OP_MUL,  9, 2, IM(35)));
    c.push_back(enc(OP_ADD,  8, 8, RG(9)));
    c.push_back(enc(OP_DIV,  9, 3, IM(10)));
    c.push_back(enc(OP_ADD,  8, 8, RG(9)));
    c.push_back(enc(OP_GT,  10, 8, IM(90)));
    c.push_back(enc(OP_GT,  11, 4, IM(30)));
    c.push_back(enc(OP_AND, 10, 10, RG(11)));
    c.push_back(enc(OP_IMM, 12, 0, IM(0x1FF)));
    c.push_back(enc(OP_SEL, 12, 10, (u16)((0 << 4) | 12)));
    c.push_back(enc(OP_JMP,  0, 0, IM(22)));

    // خروج آرام (اندیس ۲۰): فقط زمزمه‌ی نادر
    c.push_back(enc(OP_GT,  12, 3, IM(240)));         // ۲۰
    c.push_back(enc(OP_SEL, 12, 12, (u16)((0 << 4) | 12)));

    c.push_back(enc(OP_SENSE, 14, 0, SN_INBITS));     // ۲۲
    c.push_back(enc(OP_FIRE,   0, 12, RG(14)));
    c.push_back(enc(OP_HALT,   0, 0, 0));
    return c;
}

// ============================================================================
//  ۸. اجرای ماشین مجازی
// ============================================================================

struct VmResult {
    bool fired = false;
    u64  mask  = 0;
    u64  bits  = 0;
    bool fault = false;
    bool sleep = false;
    int  used  = 0;
};

static VmResult vm_run(Neuron& nu, Brain& br) {
    VmResult R;
    const Program& P = g_progs[nu.prog];
    if (P.code.empty()) { R.fault = true; return R; }

    i32 r[16]; memset(r, 0, sizeof(r));
    const int  fuel_cap = KIND_FUEL[nu.kind];
    const int  nlines   = nu.lines();
    const int  memsz    = (int)nu.mem.size();
    const bool allow_jump = (nu.kind == K_GIANT);
    const bool allow_loop = (nu.kind != K_NORMAL);
    const int  loop_cap   = (nu.kind == K_GIANT) ? 64 : 8;

    // پیش‌محاسبه‌ی حسگرها
    int inpop = 0, freshpop = 0;
    for (int i = 0; i < nlines; ++i) {
        if (nu.in_bits >> i & 1) ++inpop;
        if (nu.in_at[i] >= 0 && br.now - nu.in_at[i] < 50 * MS) ++freshpop;
    }
    const i32 manapct = (i32)(nu.cap > 0 ? (nu.mana * 100 / nu.cap) : 0);
    const i32 poolpct = (i32)(br.lp[nu.lobe].target > 0
                              ? (br.lp[nu.lobe].pool * 100 / br.lp[nu.lobe].target) : 0);
    const i32 tsf = (i32)std::min<vtime>(nu.last_fire  < 0 ? 30000 : (br.now - nu.last_fire)  / MS, 30000);
    const i32 tsi = (i32)std::min<vtime>(nu.last_input < 0 ? 30000 : (br.now - nu.last_input) / MS, 30000);

    int  pc = 0, fuel = 0;
    int  loop_pc = -1, loop_n = 0, loop_i = 0;

    // حالت فوری (بیت ۱۵ روشن) → عدد علامت‌دار ۱۵ بیتی ؛ وگرنه شماره‌ی ثبات
    auto RD = [&](u16 imm) -> i32 {
        if (!(imm & 0x8000)) return r[imm & 15];
        i32 v = (i32)(imm & 0x7FFF);
        if (v & 0x4000) v -= 0x8000;
        return v;
    };

    while (pc >= 0 && pc < (int)P.code.size() && fuel < fuel_cap) {
        ++fuel;
        const u32 ins = P.code[pc];
        const u8  op  = (u8)(ins >> 24);
        const u8  d   = (u8)((ins >> 20) & 15);
        const u8  a   = (u8)((ins >> 16) & 15);
        const u16 im  = (u16)(ins & 0xFFFF);
        ++pc;

        switch (op) {
        case OP_NOP:  break;
        case OP_IMM:  r[d] = RD(im); break;
        case OP_MOV:  r[d] = r[a];   break;
        case OP_ADD:  r[d] = r[a] + RD(im); break;
        case OP_SUB:  r[d] = r[a] - RD(im); break;
        case OP_MUL:  r[d] = (i32)(((i64)r[a] * RD(im)) & 0x7FFFFFFF); break;
        case OP_DIV: { i32 v = RD(im); r[d] = v ? r[a] / v : 0; break; }
        case OP_MOD: { i32 v = RD(im); r[d] = v ? r[a] % v : 0; break; }
        case OP_AND:  r[d] = r[a] & RD(im); break;
        case OP_OR:   r[d] = r[a] | RD(im); break;
        case OP_XOR:  r[d] = r[a] ^ RD(im); break;
        case OP_NOT:  r[d] = ~r[a]; break;
        case OP_SHL:  r[d] = (i32)((u32)r[a] << (RD(im) & 31)); break;
        case OP_SHR:  r[d] = (i32)((u32)r[a] >> (RD(im) & 31)); break;
        case OP_EQ:   r[d] = (r[a] == RD(im)); break;
        case OP_LT:   r[d] = (r[a] <  RD(im)); break;
        case OP_GT:   r[d] = (r[a] >  RD(im)); break;
        case OP_SEL:  r[d] = r[a] ? r[(im >> 4) & 15] : r[im & 15]; break;

        case OP_LD: {
            int addr = (r[a] + (i32)(im & 0x7FFF)) ;
            if (memsz) { addr = ((addr % memsz) + memsz) % memsz; r[d] = nu.mem[addr]; }
            else r[d] = 0;
            break;
        }
        case OP_ST: {
            int addr = (r[a] + (i32)(im & 0x7FFF));
            if (memsz) { addr = ((addr % memsz) + memsz) % memsz; nu.mem[addr] = (u8)(r[d] & 0xFF); }
            break;
        }

        case OP_SENSE: {
            u16 ch = im & 0x7FFF;
            i32 v = 0;
            if (ch >= SN_LINE0) {
                int li = ch - SN_LINE0;
                v = (li < nlines) ? (i32)((nu.in_bits >> li) & 1) : 0;
            } else switch (ch) {
                case SN_INPOP:    v = inpop; break;
                case SN_INBITS:   v = (i32)(nu.in_bits & 0xFFFF); break;
                case SN_FRESHPOP: v = freshpop; break;
                case SN_MANA:     v = (i32)(nu.mana / MANA); break;
                case SN_MANAPCT:  v = manapct; break;
                case SN_TSF:      v = tsf; break;
                case SN_TSI:      v = tsi; break;
                case SN_POOLPCT:  v = poolpct; break;
                case SN_NOISE: {
                    int T = br.temperature.load(std::memory_order_relaxed);
                    v = (i32)((nu.rng.u32r() & 0xFF) * T / 255);
                    break;
                }
                default: v = 0;
            }
            r[d] = v;
            break;
        }

        case OP_FIRE: {
            u64 mask = (u64)(u32)r[a];
            if (nlines < 64) mask &= ((1ull << nlines) - 1);
            if (mask) { R.fired = true; R.mask = mask; R.bits = (u64)(u32)RD(im); }
            break;
        }
        case OP_SLEEP: R.sleep = true; pc = -1; break;

        case OP_LOOP:
            if (!allow_loop) break;                       // توده: NOP
            loop_pc = pc; loop_n = std::min(RD(im), loop_cap); loop_i = 0;
            if (loop_n <= 0) {                            // پرش به ENDL
                int depth = 1;
                while (pc < (int)P.code.size() && depth) {
                    u8 o2 = (u8)(P.code[pc] >> 24);
                    if (o2 == OP_LOOP) ++depth;
                    if (o2 == OP_ENDL) --depth;
                    ++pc;
                }
            }
            break;
        case OP_ENDL:
            if (!allow_loop) break;
            if (loop_pc >= 0 && ++loop_i < loop_n) pc = loop_pc;
            else loop_pc = -1;
            break;

        case OP_JMP: if (allow_jump) pc = (int)(im & 0x7FFF); break;
        case OP_JZ:  if (allow_jump && r[a] == 0) pc = (int)(im & 0x7FFF); break;
        case OP_JNZ: if (allow_jump && r[a] != 0) pc = (int)(im & 0x7FFF); break;

        case OP_HALT: pc = -1; break;
        default: break;                                    // کد نامعتبر = NOP (بند ۹٫۱)
        }
    }

    R.used = fuel;
    if (fuel >= fuel_cap) R.fault = true;                  // سوخت تمام شد → خواب
    return R;
}

// ============================================================================
//  ۹. ساخت مغز
// ============================================================================

static void build_brain(int N, u64 seed) {
    B.seed = seed;
    B.grng = Rng(seed);
    Rng& R = B.grng;

    // --- مخزن بایت‌کد: چند گونه، تا انتخاب طبیعی چیزی برای کار داشته باشد ---
    g_progs.clear();
    for (int t = 0; t < 6; ++t)
        g_progs.push_back(Program{ seed_normal(70 + t * 22, 1 << (t % 4), 18 + t * 4), 0 });
    const u32 P_NORM0 = 0, P_NORM_N = 6;
    for (int t = 0; t < 3; ++t)
        g_progs.push_back(Program{ seed_memory(90 + t * 40), 0 });
    const u32 P_MEM0 = 6, P_MEM_N = 3;
    g_progs.push_back(Program{ seed_giant(), 0 });
    const u32 P_GIANT = 9;

    // --- جمعیت (بند ۲٫۱) ---
    int n_mem   = std::max(1, (int)llround(N * 0.03));
    int n_giant = std::max(1, n_mem / 20);
    int n_norm  = N - n_mem - n_giant;

    B.n.clear();
    B.n.reserve(N);

    const i32 GRID = (i32)std::max(4.0, std::ceil(std::sqrt((double)N)));

    auto add = [&](u8 kind, u8 lobe, u8 half) {
        Neuron nu;
        nu.id    = (u32)B.n.size();
        nu.kind  = kind;
        nu.lobe  = lobe;
        nu.half  = half;
        nu.cap   = KIND_CAP[kind] * MANA;
        nu.mana  = nu.cap / 2;
        nu.mem.assign(KIND_MEM[kind], 0);
        nu.in_at.assign(KIND_LINES[kind], -1);
        nu.rng   = Rng(R.next());
        nu.x     = (i32)R.below((u32)GRID);
        nu.y     = (i32)R.below((u32)GRID);
        nu.last_income = 0;
        if      (kind == K_NORMAL) nu.prog = P_NORM0 + R.below(P_NORM_N);
        else if (kind == K_MEMORY) nu.prog = P_MEM0  + R.below(P_MEM_N);
        else                       nu.prog = P_GIANT;
        B.n.push_back(std::move(nu));
    };

    // نسبت لوب‌ها: ورودی ۲۰٪ (نصف الف، نصف ب) · مرکزی ۶۰٪ · پایانی ۲۰٪
    for (int i = 0; i < n_norm; ++i) {
        double u = (double)i / std::max(1, n_norm);
        u8 lobe = (u < 0.20) ? L_INPUT : (u < 0.80 ? L_CENTRAL : L_OUTPUT);
        u8 half = (lobe == L_INPUT) ? (u8)(i & 1) : 0;
        add(K_NORMAL, lobe, half);
    }
    for (int i = 0; i < n_mem; ++i) {
        double u = (double)i / std::max(1, n_mem);
        u8 lobe = (u < 0.15) ? L_INPUT : (u < 0.85 ? L_CENTRAL : L_OUTPUT);
        add(K_MEMORY, lobe, (u8)(i & 1));
    }
    for (int i = 0; i < n_giant; ++i) add(K_GIANT, L_CENTRAL, 0);

    // --- سیم‌کشی محلی‌گرا (بند ۱۲٫۳): ~۹۰٪ کوتاه، ~۱۰٪ دوربرد ---
    std::vector<u32> idx_mem, idx_giant;
    for (auto& nu : B.n) {
        if (nu.kind == K_MEMORY) idx_mem.push_back(nu.id);
        if (nu.kind == K_GIANT)  idx_giant.push_back(nu.id);
    }

    auto pick_local = [&](const Neuron& src) -> u32 {
        // نمونه‌گیری چندباره و انتخاب نزدیک‌ترین → افت احتمال با فاصله
        u32 best = R.below((u32)B.n.size());
        i64 bd = INT64_MAX;
        for (int k = 0; k < 6; ++k) {
            u32 c = R.below((u32)B.n.size());
            if (c == src.id) continue;
            i64 dx = B.n[c].x - src.x, dy = B.n[c].y - src.y;
            i64 dd = dx * dx + dy * dy;
            if (dd < bd) { bd = dd; best = c; }
        }
        return best;
    };

    for (auto& nu : B.n) {
        const int nl = nu.lines();
        if (nu.kind == K_GIANT) {
            // ۲۰ خط → حافظه‌ای‌های زیردست · ۴۰ خط → غول‌های دیگر (بند ۲٫۲)
            for (int k = 0; k < 20 && !idx_mem.empty(); ++k) {
                u32 dst = idx_mem[R.below((u32)idx_mem.size())];
                B.n[nu.id].out.push_back(Edge{dst, (u8)R.below((u32)B.n[dst].lines()),
                                              R.range(EDGE_MIN, EDGE_MAX)});
            }
            for (int k = 0; k < 40 && idx_giant.size() > 1; ++k) {
                u32 dst = idx_giant[R.below((u32)idx_giant.size())];
                if (dst == nu.id) continue;
                B.n[nu.id].out.push_back(Edge{dst, (u8)R.below((u32)B.n[dst].lines()),
                                              R.range(EDGE_MIN, EDGE_MAX)});
            }
            continue;
        }
        for (int k = 0; k < nl; ++k) {
            u32 dst;
            if (R.below(100) < 90) dst = pick_local(nu);          // محلی
            else                   dst = R.below((u32)B.n.size()); // بزرگراه
            if (dst == nu.id) continue;                            // A→A ممنوع (بند ۴)
            if (B.n[dst].kind == K_GIANT) continue;                // غول فقط از نخبه‌ها می‌شنود
            nu.out.push_back(Edge{dst, (u8)R.below((u32)B.n[dst].lines()),
                                  R.range(EDGE_MIN, EDGE_MAX)});
        }
    }

    // --- استخرها (بند ۵٫۳) ---
    for (int L = 0; L < N_LOBES; ++L) { B.lp[L] = LobePool{}; }
    for (auto& nu : B.n) { B.lp[nu.lobe].alive++; B.lp[nu.lobe].cap_sum += nu.cap; }
    for (int L = 0; L < N_LOBES; ++L) {
        B.lp[L].target = B.lp[L].cap_sum * 3 / 5;      // هدف = ۰٫۶ برابر — کمیابی واقعی
        B.lp[L].pool   = B.lp[L].target / 2;
    }
    B.treasury = 0;

    // --- زمان‌بندی اولیه ---
    B.now = 0; B.seq = 0;
    while (!B.q.empty()) B.q.pop();
    for (auto& nu : B.n)
        B.push(R.range(0, KIND_CADENCE[nu.kind]), nu.id, EV_EVAL);
    B.push(SYS_TICK, 0, EV_SYS);

    B.transit.clear(); B.transit_total = 0;
    B.c_fires = B.c_signals = B.c_faults = B.c_events = 0;
    B.c_fires_prev = 0; B.t_prev = 0;
    B.out_bits.clear(); B.out_text.clear();
}

// ============================================================================
//  ۱۰. اقتصاد
// ============================================================================

// برداشت از استخر لوب تا سقف مخزن
static void draw_from_pool(Neuron& nu) {
    LobePool& L = B.lp[nu.lobe];
    i64 want = nu.cap - nu.mana;
    if (want <= 0 || L.pool <= 0) return;
    i64 got = std::min(want, L.pool);
    // سهم به نسبت اعتبار: نورون بی‌اعتبار کندتر پر می‌شود (بند ۵٫۳)
    i64 share = 15 + std::min<i64>(85, (i64)nu.credit / 180);   // ۱۵٪..۱۰۰٪ — اعتبار تعیین‌کننده
    got = got * share / 100;
    if (got <= 0) return;
    L.pool  -= got;
    nu.mana += got;
    nu.last_income = B.now;
}

static void burn(i64 amount) {                 // مانا می‌سوزد و پس از تأخیر برمی‌گردد
    B.transit.push_back({B.now + TRANSIT_TIME, amount});
    B.transit_total += amount;
}

static void apply_reward(i64 milli) {          // پاداش/تنبیه از دستگاه (بند ۵٫۳)
    if (milli == 0) return;
    // فشرده‌سازی لگاریتمی + کف حیاتی (بند ۱۱٫۲)
    double s = (double)milli;
    double c = (s >= 0 ? 1.0 : -1.0) * std::log(1.0 + std::fabs(s) / MANA) * 3.0 * MANA;
    i64 v = (i64)c;

    // نشت خودتنظیم (بند ۵٫۳): استخر گرسنه بیشتر می‌مکد
    auto hunger = [&](int L) -> double {
        double f = (double)B.lp[L].pool / std::max<i64>(1, B.lp[L].target);
        return std::max(0.0, std::min(1.0, 1.0 - f));
    };
    i64 to_out = v;
    i64 back1  = (i64)(v * (LEAK_BACK / 100.0) * (1.0 + hunger(L_CENTRAL)));
    i64 back2  = (i64)(back1 * (LEAK_BACK / 100.0) * (1.0 + hunger(L_INPUT)));
    i64 fwd    = v * LEAK_FWD / 100;

    B.lp[L_OUTPUT].pool  += to_out;
    B.lp[L_CENTRAL].pool += back1;
    B.lp[L_INPUT].pool   += back2 + fwd;

    // کف حیاتی: تنبیه نمی‌تواند استخر را زیر ۵٪ هدف ببرد (بند ۱۱٫۲)
    for (int L = 0; L < N_LOBES; ++L) {
        i64 floor_ = B.lp[L].target / 20;
        if (B.lp[L].pool < floor_) B.lp[L].pool = floor_;
    }
}

// ============================================================================
//  ۱۱. حلقه‌ی رویداد
// ============================================================================

static void deliver(u32 dst, u8 line, u8 bit) {
    Neuron& t = B.n[dst];
    if (t.state == S_DEAD) return;
    if (line >= t.lines()) return;
    if (bit) t.in_bits |=  (1ull << line);
    else     t.in_bits &= ~(1ull << line);
    t.in_at[line] = B.now;
    t.last_input  = B.now;
    if (t.state == S_DORMANT) t.state = S_HEALTHY;          // بیدار شدن (بند ۵٫۴)
}

static void neuron_eval(u32 id) {
    Neuron& nu = B.n[id];
    if (nu.state == S_DEAD) return;

    const vtime dt = B.now - nu.last_eval;
    nu.last_eval = B.now;

    // --- هزینه‌ی زنده‌ماندن (بند ۵٫۴) ---
    i64 upkeep = nu.cap * UPKEEP_PCT / 1000 * dt / SEC;
    if (nu.state == S_DORMANT) upkeep /= 10;
    nu.mana -= upkeep;
    burn(upkeep > 0 ? upkeep : 0);

    // --- تلاش برای پر کردن مخزن ---
    if (nu.mana < nu.cap) draw_from_pool(nu);

    // --- محو تدریجی اعتبار (شیفت ارزان، نه تقسیم) ---
    {
        i64 decay = (i64)nu.credit * dt / (5 * SEC);
        nu.credit = (u16)((i64)nu.credit > decay ? (i64)nu.credit - decay : 0);
    }

    // --- حالت‌ها (بند ۶) ---
    if (nu.mana <= 0) {
        nu.mana = 0;
        if (nu.state != S_SPAM && B.now - nu.last_income > STARVE_TIME) {
            nu.state = S_SPAM;
            nu.dcredit = DEATH_CREDIT;
            nu.spam_until = B.now + SPAM_TIME;
        }
    } else if (nu.mana * 100 < nu.cap * IGNORE_PCT) {
        if (nu.state == S_HEALTHY || nu.state == S_DORMANT) nu.state = S_IGNORE;
    } else {
        if (nu.state == S_IGNORE) nu.state = S_HEALTHY;
    }

    if (nu.state == S_HEALTHY &&
        nu.last_input >= 0 && B.now - nu.last_input > DORMANT_TIME &&
        nu.last_fire  >= 0 && B.now - nu.last_fire  > DORMANT_TIME) {
        nu.state = S_DORMANT;
    }

    vtime cadence = KIND_CADENCE[nu.kind];

    // ---------------------- فاز اسپم -----------------------------------
    if (nu.state == S_SPAM) {
        if (B.now >= nu.spam_until || nu.dcredit <= 0) {
            nu.state = S_DEAD;
            B.lp[nu.lobe].alive--;
            B.lp[nu.lobe].cap_sum -= nu.cap;
            B.lp[nu.lobe].deaths_window++;
            return;
        }
        // اسپم از اعتبار پایانی خرج می‌شود، نه از استخر (بند ۶)
        int nl = nu.lines();
        u64 mask = nu.rng.next() & ((nl >= 64) ? ~0ull : ((1ull << nl) - 1));
        int cnt = __builtin_popcountll(mask);
        i64 cost = FIRE_STARTUP + FIRE_PER_LINE * cnt;
        if (cost > nu.dcredit) { nu.dcredit = 0; }
        else {
            nu.dcredit -= cost;
            u64 bits = nu.rng.next();
            for (auto& e : nu.out) {
                if (e.line < 64 && ((mask >> (e.line % nl)) & 1)) {
                    B.push(B.now + e.delay, e.dst, EV_SIGNAL, e.line, (u8)((bits >> (e.line & 63)) & 1));
                    B.c_signals++;
                }
            }
        }
        B.push(B.now + cadence, id, EV_EVAL);
        return;
    }

    if (nu.state == S_IGNORE) {                    // ورودی می‌گیرد، فایر نمی‌کند
        B.push(B.now + cadence, id, EV_EVAL);
        return;
    }
    if (nu.state == S_ASLEEP) {                    // خواب پس از خطا — منتظر نجات غول
        B.push(B.now + cadence * 4, id, EV_EVAL);
        return;
    }
    if (nu.state == S_DORMANT) cadence *= 10;      // خواب زمستانی

    // ---------------------- اجرای تابع ----------------------------------
    VmResult res = vm_run(nu, B);

    if (res.fault) {
        nu.faults++; B.c_faults++;
        nu.state = S_ASLEEP;                       // بند ۹
        B.push(B.now + cadence * 4, id, EV_EVAL);
        return;
    }
    if (res.sleep) { B.push(B.now + cadence * 3, id, EV_EVAL); return; }

    // دوره‌ی تعلیق: نورون بلافاصله پس از فایر نمی‌تواند دوباره شلیک کند
    bool refractory = (nu.last_fire >= 0 && B.now - nu.last_fire < REFRACTORY);

    if (res.fired && res.mask && !refractory) {
        int nl  = nu.lines();
        int cnt = __builtin_popcountll(res.mask);
        i64 cost = FIRE_STARTUP + FIRE_PER_LINE * cnt;     // بند ۲٫۴
        if (nu.mana >= cost) {
            nu.mana -= cost;
            burn(cost);
            nu.last_fire = B.now;
            nu.fires++; B.c_fires++;
            {   // اعتبار = سرمایه‌گذاری؛ اشباع در ۶۵۵۳۵
                i64 nc = (i64)nu.credit + cost / 16;
                nu.credit = (u16)std::min<i64>(nc, 65535);
            }

            for (auto& e : nu.out) {
                int li = e.line % nl;
                if ((res.mask >> li) & 1) {
                    u8 bit = (u8)((res.bits >> (li & 63)) & 1);
                    B.push(B.now + e.delay, e.dst, EV_SIGNAL, e.line, bit);
                    B.c_signals++;
                }
            }
            // خروجی لوب پایانی → دستگاه (بند ۱۱٫۱)
            // ۲ خط پایانی هر نورون خروجی، به خروجی واقعی تبدیل می‌شود.
            if (nu.lobe == L_OUTPUT) {
                int l0 = nl - 2, l1 = nl - 1;
                bool a0 = (res.mask >> l0) & 1, a1 = (res.mask >> l1) & 1;
                if (a0 || a1) {
                    B.out_bits.push_back((u8)((res.bits >> l0) & 1));
                    B.out_bits.push_back((u8)((res.bits >> l1) & 1));
                }
                if (B.out_bits.size() > 4096) B.out_bits.erase(B.out_bits.begin(), B.out_bits.begin() + 2048);
            }
        }
        // پس از فایر، بیت تازگی پاک می‌شود
        nu.in_bits = 0;
    }

    // بی‌قراری: هرچه مانا بیشتر، سریع‌تر دوباره فکر می‌کند
    vtime jitter = (vtime)(nu.rng.below((u32)(cadence / 2)));
    B.push(B.now + cadence / 2 + jitter, id, EV_EVAL);
}

static void system_tick() {
    // --- بازگشت مانای در ترانزیت (بند ۵٫۲) ---
    while (!B.transit.empty() && B.transit.front().first <= B.now) {
        i64 amt = B.transit.front().second;
        B.transit.pop_front();
        B.transit_total -= amt;
        // به استخرها به نسبت جمعیت زنده برمی‌گردد
        // بازگشت به نسبت ظرفیت، ولی هرگز فراتر از هدف — مازاد به خزانه (ضدتورم)
        i64 tot = B.lp[0].cap_sum + B.lp[1].cap_sum + B.lp[2].cap_sum;
        if (tot > 0) {
            for (int L = 0; L < N_LOBES; ++L) {
                i64 part = amt * B.lp[L].cap_sum / tot;
                i64 room = B.lp[L].target - B.lp[L].pool;
                if (room < 0) room = 0;
                i64 give = std::min(part, room);
                B.lp[L].pool += give;
                B.treasury   += part - give;
            }
        } else B.treasury += amt;
    }

    // --- درآمد پایه + نشت خودتنظیم (بند ۵٫۳) ---
    // اصلاح مرحله‌ی ۰: درآمد باید با ظرفیت وزن بخورد، نه سرانه‌ی تخت.
    // با درآمد تخت، نورون حافظه‌ای (هزینه ۰٫۸/s) و غول (۲٫۴/s) از روز اول
    // ورشکسته‌ی ساختاری‌اند و می‌میرند — که در اولین اجرا دقیقاً رخ داد.
    for (int L = 0; L < N_LOBES; ++L) {
        LobePool& P = B.lp[L];
        i64 income = P.cap_sum * BASE_INCOME / (20 * MANA) * SYS_TICK / SEC;
        // جبران گرسنگی: هرچه استخر خالی‌تر، درآمد بیشتر (تا ۴ برابر)
        double f = (double)P.pool / std::max<i64>(1, P.target);
        if (f < 0.25) income = (i64)(income * (1.0 + 0.5 * (0.25 - f) / 0.25));
        if (P.emergency) income *= 3;
        // هومئوستاز: خزانه فقط تا سقف هدف پر می‌کند — جلوگیری از تورم
        i64 room = P.target - P.pool;
        if (room < 0) room = 0;
        P.pool += std::min(income, room);
    }

    // --- سقف نرخ مرگ (بند ۶) ---
    for (int L = 0; L < N_LOBES; ++L) {
        LobePool& P = B.lp[L];
        P.target = std::max<i64>(1, P.cap_sum * 3 / 5);
        i64 cap_deaths = std::max<i64>(1, P.alive * DEATH_CAP_PPT / 1000);
        P.emergency = (P.deaths_window > cap_deaths);
        P.deaths_window = 0;
    }

    // --- پاداش معلق از دستگاه ---
    i64 rw = g_reward_pending.exchange(0);
    if (rw) apply_reward(rw);

    B.push(B.now + SYS_TICK, 0, EV_SYS);
}

// ============================================================================
//  ۱۲. دستگاه — کدک فارسی (بند ۱۱٫۱)
// ============================================================================

// ---------------------------------------------------------------------------
//  دستگاه — بخش زبان: بیت → بایت → کلمه (بند ۱۱٫۱)
//  فاصله جداکننده‌ی کلمات است و به کلمه‌ی قبلش می‌چسبد.
// ---------------------------------------------------------------------------
// بستن کلمه‌ی جاری و فرستادنش برای نمره‌دهی
static void device_close_word() {
    if (B.cur_word.empty()) return;
    OutWord w;
    w.id = B.next_word_id++;
    w.text = B.cur_word + " ";           // فاصله به کلمه می‌چسبد (بند ۱۱٫۱)
    w.t = (double)B.now / SEC;
    w.score = 0; w.scored = false;
    B.words.push_back(w);
    B.words_total++;
    if (B.words.size() > 250) B.words.erase(B.words.begin());
    B.cur_word.clear();
}

static void device_decode() {
    while (B.out_bits.size() >= 8) {
        u8 byte = 0;
        for (int i = 0; i < 8; ++i) byte = (u8)((byte << 1) | B.out_bits[i]);
        B.out_bits.erase(B.out_bits.begin(), B.out_bits.begin() + 8);

        // آینه: هرچه گفت با تأخیر به نیمه‌ی ب لوب ورودی برمی‌گردد (بند ۴)
        for (int i = 7; i >= 0; --i) B.mirror_queue.push_back((u8)((byte >> i) & 1));

        // ---- رمزگشای UTF-8 چندبایتی (اصلاح: فارسی دوبایتی است) ----
        if (B.utf8_need > 0) {
            if ((byte & 0xC0) == 0x80) {                 // بایت ادامه معتبر
                B.utf8_buf.push_back((char)byte);
                if (--B.utf8_need == 0) {
                    B.cur_word += B.utf8_buf;
                    B.out_text += B.utf8_buf;
                    B.utf8_buf.clear();
                    B.chars_ok++;
                    if (B.cur_word.size() >= 16) device_close_word();
                }
                continue;
            }
            B.utf8_buf.clear(); B.utf8_need = 0;          // دنباله‌ی خراب
            B.chars_bad++;
        }

        if (byte < 0x80) {
            // ---- ASCII ----
            bool is_space = (byte == 32 || byte == 10 || byte == 13 || byte == 9);
            if (is_space) { device_close_word(); B.out_text.push_back(' '); }
            else if (byte >= 32) {
                B.cur_word.push_back((char)byte);
                B.out_text.push_back((char)byte);
                B.chars_ok++;
                if (B.cur_word.size() >= 16) device_close_word();
            } else B.chars_bad++;
        } else if ((byte & 0xE0) == 0xC0) {
            B.utf8_buf.assign(1, (char)byte); B.utf8_need = 1;   // دوبایتی (فارسی)
        } else if ((byte & 0xF0) == 0xE0) {
            B.utf8_buf.assign(1, (char)byte); B.utf8_need = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            B.utf8_buf.assign(1, (char)byte); B.utf8_need = 3;
        } else {
            B.chars_bad++;                                        // بایت نامعتبر
        }
    }
    if (B.out_text.size() > 600) B.out_text.erase(0, B.out_text.size() - 600);
}

// ---------------------------------------------------------------------------
//  دستگاه — بخش حواس: متن انسان → بیت → نیمه‌ی الف لوب ورودی
// ---------------------------------------------------------------------------
static void device_say(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mx);
    ChatMsg m;
    m.id = B.next_msg_id++;
    m.from_human = true;
    m.text = text;
    m.t = (double)B.now / SEC;
    B.chat.push_back(m);
    if (B.chat.size() > 120) B.chat.erase(B.chat.begin());

    for (unsigned char ch : text)
        for (int i = 7; i >= 0; --i) B.in_queue.push_back((u8)((ch >> i) & 1));
    B.in_queue.push_back(0); B.in_queue.push_back(0);   // مکث
    for (int i = 7; i >= 0; --i) B.in_queue.push_back((u8)((32 >> i) & 1)); // فاصله‌ی پایانی
}

// تزریق بیت‌ها به دو خط پایانی نورون‌های نیمه‌ی مربوطه‌ی لوب ورودی
static void device_inject() {
    if (B.now < B.next_inject) return;
    B.next_inject = B.now + 8 * MS;      // ~۱۲۵ بیت در ثانیه‌ی مجازی

    auto feed = [&](std::deque<u8>& q, u8 half) {
        if (q.empty()) return;
        u8 b0 = q.front(); q.pop_front();
        u8 b1 = 0;
        if (!q.empty()) { b1 = q.front(); q.pop_front(); }
        int fed = 0;
        for (auto& nu : B.n) {
            if (nu.lobe != L_INPUT || nu.half != half || nu.state == S_DEAD) continue;
            int nl = nu.lines();
            deliver(nu.id, (u8)(nl - 2), b0);
            deliver(nu.id, (u8)(nl - 1), b1);
            if (++fed >= 64) break;      // به یک زیرمجموعه تزریق می‌شود، نه همه
        }
    };
    feed(B.in_queue,     0);             // نیمه‌ی الف — از انسان
    feed(B.mirror_queue, 1);             // نیمه‌ی ب  — آینه‌ی خود مدل
}

// ---------------------------------------------------------------------------
//  دستگاه — داور: نمره‌ی کلمه → مانا (بند ۱۱٫۲)
// ---------------------------------------------------------------------------
static void device_score(u32 word_id, int score) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& w : B.words) {
        if (w.id != word_id) continue;
        if (w.scored) B.score_sum -= w.score; else B.words_scored++;
        w.score  = score;
        w.scored = true;
        B.score_sum += score;
        g_reward_pending.fetch_add((i64)score * MANA);
        return;
    }
}

// ============================================================================
//  ۱۳. چک‌پوینت  —  brain.dat  (بند ۱۲٫۸)
// ============================================================================

static bool save_brain(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const char magic[8] = {'S','A','R','I','N','A','0','3'};
    fwrite(magic, 1, 8, f);
    u32 nprog = (u32)g_progs.size(); fwrite(&nprog, 4, 1, f);
    for (auto& p : g_progs) {
        u32 len = (u32)p.code.size(); fwrite(&len, 4, 1, f);
        fwrite(p.code.data(), 4, len, f);
    }
    u32 nn = (u32)B.n.size(); fwrite(&nn, 4, 1, f);
    fwrite(&B.now, 8, 1, f);
    fwrite(&B.seed, 8, 1, f);
    for (auto& nu : B.n) {
        fwrite(&nu.id, 4, 1, f);
        fwrite(&nu.kind, 1, 1, f); fwrite(&nu.lobe, 1, 1, f);
        fwrite(&nu.state, 1, 1, f); fwrite(&nu.half, 1, 1, f);
        fwrite(&nu.x, 4, 1, f); fwrite(&nu.y, 4, 1, f);
        fwrite(&nu.mana, 8, 1, f); fwrite(&nu.cap, 8, 1, f);
        fwrite(&nu.credit, 2, 1, f);
        fwrite(&nu.last_fire, 8, 1, f); fwrite(&nu.last_input, 8, 1, f);
        fwrite(&nu.prog, 4, 1, f);
        fwrite(&nu.rng.s, 8, 1, f);
        fwrite(&nu.in_bits, 8, 1, f);
        u32 msz = (u32)nu.mem.size(); fwrite(&msz, 4, 1, f);
        fwrite(nu.mem.data(), 1, msz, f);
        u32 esz = (u32)nu.out.size(); fwrite(&esz, 4, 1, f);
        for (auto& e : nu.out) { fwrite(&e.dst,4,1,f); fwrite(&e.line,1,1,f); fwrite(&e.delay,8,1,f); }
    }
    for (int L = 0; L < N_LOBES; ++L) {
        fwrite(&B.lp[L].pool, 8, 1, f);
        fwrite(&B.lp[L].alive, 8, 1, f);
    }
    fclose(f);
    return true;
}

static bool load_brain(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    char magic[8];
    if (fread(magic,1,8,f) != 8 || memcmp(magic,"SARINA03",8)) { fclose(f); return false; }
    u32 nprog = 0; if (fread(&nprog,4,1,f)!=1) { fclose(f); return false; }
    g_progs.clear(); g_progs.resize(nprog);
    for (u32 i = 0; i < nprog; ++i) {
        u32 len = 0; if (fread(&len,4,1,f)!=1) { fclose(f); return false; }
        g_progs[i].code.resize(len);
        if (len && fread(g_progs[i].code.data(),4,len,f)!=len) { fclose(f); return false; }
    }
    u32 nn = 0;
    if (fread(&nn,4,1,f)!=1 || fread(&B.now,8,1,f)!=1 || fread(&B.seed,8,1,f)!=1) { fclose(f); return false; }
    B.n.clear(); B.n.resize(nn);
    for (u32 i = 0; i < nn; ++i) {
        Neuron& nu = B.n[i];
        fread(&nu.id,4,1,f);
        fread(&nu.kind,1,1,f); fread(&nu.lobe,1,1,f);
        fread(&nu.state,1,1,f); fread(&nu.half,1,1,f);
        fread(&nu.x,4,1,f); fread(&nu.y,4,1,f);
        fread(&nu.mana,8,1,f); fread(&nu.cap,8,1,f);
        fread(&nu.credit,2,1,f);
        fread(&nu.last_fire,8,1,f); fread(&nu.last_input,8,1,f);
        fread(&nu.prog,4,1,f);
        fread(&nu.rng.s,8,1,f);
        fread(&nu.in_bits,8,1,f);
        u32 msz=0; fread(&msz,4,1,f); nu.mem.resize(msz);
        if (msz) fread(nu.mem.data(),1,msz,f);
        u32 esz=0; fread(&esz,4,1,f); nu.out.resize(esz);
        for (u32 k=0;k<esz;++k){ fread(&nu.out[k].dst,4,1,f); fread(&nu.out[k].line,1,1,f); fread(&nu.out[k].delay,8,1,f); }
        nu.in_at.assign(KIND_LINES[nu.kind], -1);
        nu.last_eval = B.now;
        nu.last_income = B.now;
    }
    for (int L = 0; L < N_LOBES; ++L) {
        fread(&B.lp[L].pool,8,1,f);
        fread(&B.lp[L].alive,8,1,f);
        B.lp[L].cap_sum = 0;
        for (auto& nu : B.n) if (nu.state != S_DEAD && nu.lobe == L) B.lp[L].cap_sum += nu.cap;
        B.lp[L].target = std::max<i64>(1, B.lp[L].cap_sum * 3 / 5);
    }
    fclose(f);
    while (!B.q.empty()) B.q.pop();
    B.seq = 0;
    Rng r(B.seed ^ 0xABCDEF);
    for (auto& nu : B.n)
        if (nu.state != S_DEAD) B.push(B.now + r.range(0, KIND_CADENCE[nu.kind]), nu.id, EV_EVAL);
    B.push(B.now + SYS_TICK, 0, EV_SYS);
    B.transit.clear(); B.transit_total = 0;
    return true;
}

// ============================================================================
//  ۱۴. حلقه‌ی شبیه‌سازی
// ============================================================================

static void snapshot(double wall) {
    Stats s;
    s.vtime_us = B.now;
    s.wall_s = wall;
    for (auto& nu : B.n) {
        if (nu.state == S_DEAD) { s.dead++; continue; }
        s.alive++;
        s.alive_lobe[nu.lobe]++;
        switch (nu.state) {
            case S_HEALTHY: s.healthy++;  break;
            case S_IGNORE:  s.ignoring++; break;
            case S_SPAM:    s.spamming++; break;
            case S_DORMANT: s.dormant++;  break;
            case S_ASLEEP:  s.asleep++;   break;
        }
        s.total_mana += nu.mana;
    }
    for (int L = 0; L < N_LOBES; ++L) {
        s.pool[L] = B.lp[L].pool;
        s.ptgt[L] = std::max<i64>(1, B.lp[L].target);
        s.total_mana += B.lp[L].pool;
    }
    s.transit = B.transit_total;
    s.total_mana += B.transit_total;
    s.fires = B.c_fires; s.signals = B.c_signals; s.faults = B.c_faults;
    s.events = B.c_events;
    vtime dv = B.now - B.t_prev;
    if (dv > 0) s.fire_hz = (double)(B.c_fires - B.c_fires_prev) * SEC / dv / std::max<i64>(1, s.alive);
    s.out_text = B.out_text;
    // ۴۰ کلمه‌ی آخر برای نمره‌دهی
    size_t wstart = B.words.size() > 40 ? B.words.size() - 40 : 0;
    s.words.assign(B.words.begin() + wstart, B.words.end());
    size_t cstart = B.chat.size() > 40 ? B.chat.size() - 40 : 0;
    s.chat.assign(B.chat.begin() + cstart, B.chat.end());
    s.words_total  = B.words_total;
    s.words_scored = B.words_scored;
    s.avg_score    = B.words_scored ? B.score_sum / B.words_scored : 0.0;

    std::lock_guard<std::mutex> lk(g_mx);
    s.hist_fire  = g_stats.hist_fire;
    s.hist_pool  = g_stats.hist_pool;
    s.hist_alive = g_stats.hist_alive;
    double poolpct = 0;
    for (int L = 0; L < N_LOBES; ++L) poolpct += (double)s.pool[L] / s.ptgt[L];
    poolpct /= N_LOBES;
    s.hist_fire.push_back(s.fire_hz);
    s.hist_pool.push_back(poolpct);
    s.hist_alive.push_back((double)s.alive);
    if (s.hist_fire.size()  > 180) s.hist_fire.erase(s.hist_fire.begin());
    if (s.hist_pool.size()  > 180) s.hist_pool.erase(s.hist_pool.begin());
    if (s.hist_alive.size() > 180) s.hist_alive.erase(s.hist_alive.begin());
    g_stats = std::move(s);
}

static void sim_loop() {
    using clk = std::chrono::steady_clock;
    auto  t0  = clk::now();
    vtime v0  = B.now;
    vtime next_snap = B.now;
    vtime next_dec  = B.now;

    while (g_running.load()) {
        if (g_paused.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            t0 = clk::now(); v0 = B.now;
            continue;
        }
        if (B.q.empty()) { std::this_thread::sleep_for(std::chrono::milliseconds(10)); continue; }

        Event e = B.q.top();
        if (g_stop_at && e.t > g_stop_at) {      // توقف قطعی در مرز رویداد
            device_decode();
            snapshot(0.0);
            g_running.store(false);
            break;
        }

        // کنترل سرعت: زمان مجازی نسبت به ساعت دیوار
        int sp = g_speed.load();
        if (sp > 0) {
            double target_wall = (double)(e.t - v0) / SEC * 1000.0 / (sp / 1000.0);
            double now_wall = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
            if (target_wall > now_wall + 1.0) {
                double slp = std::min(50.0, target_wall - now_wall);
                std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(slp));
                continue;
            }
        }

        B.q.pop();
        B.now = e.t;
        B.c_events++;

        switch (e.type) {
            case EV_EVAL:   neuron_eval(e.target); break;
            case EV_SIGNAL: deliver(e.target, e.line, e.bit); break;
            case EV_SYS:    system_tick(); break;
            default: break;
        }

        device_inject();

        if (B.now >= next_snap) {
            device_decode();
            double wall = std::chrono::duration<double>(clk::now() - t0).count();
            snapshot(wall);
            B.c_fires_prev = B.c_fires; B.t_prev = B.now;
            next_snap = B.now + 200 * MS;
        }
        if (B.now >= next_dec) {                       // کاهش تدریجی دما
            int T = B.temperature.load();
            if (T > 20) B.temperature.store(T - 1);
            next_dec = B.now + 5 * SEC;
        }
        if (g_shutdown_req.load()) {
            save_brain("brain.dat");
            g_shutdown_req.store(false);
            g_paused.store(true);
        }
    }
}

// ============================================================================
//  ۱۵. داشبورد  —  وب‌سرور کوچک + HTML درون‌خطی (بند ۱۲٫۷)
// ============================================================================

static const char* PAGE = R"HTML(<!DOCTYPE html>
<html lang="fa" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>سارینا — مرحله‌ی صفر</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0a0d12;color:#dfe7f0;font:14px/1.7 Vazirmatn,Tahoma,system-ui,sans-serif}
header{padding:14px 20px;background:#111722;border-bottom:1px solid #1e2836;display:flex;
       align-items:center;gap:16px;flex-wrap:wrap;position:sticky;top:0;z-index:9}
h1{margin:0;font-size:17px;font-weight:700;letter-spacing:.3px}
.tag{font-size:11px;padding:3px 9px;border-radius:20px;background:#16202e;color:#7f9ec4;border:1px solid #223046}
.wrap{padding:18px;max-width:1500px;margin:0 auto}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(215px,1fr));gap:12px;margin-bottom:16px}
.card{background:#111722;border:1px solid #1c2634;border-radius:11px;padding:13px 15px}
.card .lbl{font-size:11px;color:#75879e;margin-bottom:5px;letter-spacing:.3px}
.card .val{font-size:25px;font-weight:700;font-variant-numeric:tabular-nums;line-height:1.2}
.card .sub{font-size:11px;color:#5d7086;margin-top:3px}
.sub{font-size:11px;color:#5d7086;margin-top:3px}
.ok{color:#3ddc84}.warn{color:#ffc857}.bad{color:#ff6b6b}.dim{color:#65788f}
.row{display:grid;grid-template-columns:1fr 1fr;gap:12px}
@media(max-width:980px){.row{grid-template-columns:1fr}}
.panel{background:#111722;border:1px solid #1c2634;border-radius:11px;padding:15px;margin-bottom:14px}
.panel h3{margin:0 0 12px;font-size:13px;color:#8fa6c2;font-weight:600}
canvas{width:100%;height:110px;display:block}
.bar{height:26px;background:#0c1119;border-radius:6px;overflow:hidden;position:relative;margin:5px 0 11px;
     border:1px solid #1b2534}
.bar i{display:block;height:100%;transition:width .3s}
.bar span{position:absolute;inset:0;display:flex;align-items:center;justify-content:space-between;
          padding:0 10px;font-size:11px;font-variant-numeric:tabular-nums}
button{background:#1b2637;color:#dfe7f0;border:1px solid #2a3a52;
       border-radius:7px;padding:7px 14px;cursor:pointer;font-family:inherit;font-size:13px}
button:hover{background:#22334a}
button.p{background:#1e5f3f;border-color:#2b7d55}
button.d{background:#5f2020;border-color:#7d2b2b}
input[type=range]{width:130px;vertical-align:middle}
.states{display:flex;gap:7px;flex-wrap:wrap}
.st{padding:4px 11px;border-radius:6px;font-size:12px;background:#141d29;border:1px solid #1e2a3a}
.out{background:#080b10;border:1px solid #1b2534;border-radius:8px;padding:12px;min-height:76px;
     font-family:monospace;direction:ltr;text-align:left;font-size:12px;color:#8de0a8;
     word-break:break-all;white-space:pre-wrap}
.chatpanel{position:relative}
.chatstats{display:flex;gap:16px;flex-wrap:wrap;font-size:11px;color:#75879e;margin-bottom:10px}
.chatstats b{color:#dfe7f0;font-variant-numeric:tabular-nums}
.chatstats .auto{margin-right:auto;cursor:pointer;user-select:none}
.stream{background:#080b10;border:1px solid #1b2534;border-radius:8px;padding:12px;
        height:300px;overflow-y:auto;font-size:14px;line-height:2.4}
.stream::-webkit-scrollbar{width:9px}
.stream::-webkit-scrollbar-thumb{background:#243349;border-radius:5px}
.hw{display:block;background:#152a3f;border:1px solid #24466b;border-radius:8px;
    padding:7px 12px;margin:7px 0;color:#a8d0ff;direction:rtl}
.hw::before{content:'تو: ';color:#5d7086;font-size:11px}
.w{display:inline-block;padding:2px 8px;margin:2px 3px;border-radius:6px;cursor:pointer;
   background:#121a26;border:1px solid #1e2a3a;font-family:monospace;direction:ltr;
   transition:.12s;font-size:13px}
.w:hover{background:#1d2b3e;border-color:#3a5a80;transform:translateY(-1px)}
.w.sc{border-color:#2b7d55;background:#0f2a1c;color:#8de0a8}
.w.sn{border-color:#7d2b2b;background:#2a1010;color:#ff9b9b}
.w .b{font-size:10px;opacity:.85;margin-right:5px;font-family:sans-serif}
.composer{display:flex;gap:8px;margin-top:11px}
.composer input{flex:1;background:#0c1119;border:1px solid #1e2a3a;border-radius:7px;
                padding:9px 12px;color:#dfe7f0;font-family:inherit;font-size:13px;direction:rtl}
.composer input:focus{outline:none;border-color:#2a4a70}
.hint{font-size:11px;color:#5d7086;margin-top:8px}
.pop{display:none;position:fixed;z-index:99;background:#141d2b;border:1px solid #2a3a52;
     border-radius:11px;padding:13px;box-shadow:0 12px 40px #000a}
.pop.on{display:block}
.popw{font-family:monospace;direction:ltr;color:#8de0a8;margin-bottom:10px;font-size:14px;
      background:#080b10;padding:7px 10px;border-radius:6px;text-align:center}
.popr{display:grid;grid-template-columns:repeat(4,1fr);gap:5px;margin-bottom:7px}
.popr button{padding:7px 4px;font-size:12px}
.popr button[data-s^="-"]{background:#3a1616;border-color:#6b2b2b}
.popr2{display:flex;gap:5px}
.popr2 input{flex:1;background:#0c1119;border:1px solid #1e2a3a;border-radius:7px;
             padding:6px 9px;color:#dfe7f0;font-family:inherit;font-size:12px;width:90px}
.verdict{padding:11px 15px;border-radius:9px;margin-bottom:14px;font-size:13px;font-weight:600}
.v-ok{background:#0f2a1c;border:1px solid #1e5f3f;color:#7fe0a8}
.v-warn{background:#2a2410;border:1px solid #6b5a1e;color:#ffd97a}
.v-bad{background:#2a1010;border:1px solid #6b1e1e;color:#ff9b9b}
</style></head><body>
<header>
  <h1>سارینا</h1>
  <span class="tag">مرحله‌ی صفر</span>
  <span class="tag" id="vt">—</span>
  <span style="flex:1"></span>
  <button id="pause">توقف</button>
  <label class="dim">سرعت <input type="range" id="spd" min="0" max="20" value="10"><b id="spdv">۱×</b></label>
  <label class="dim">دما <input type="range" id="tmp" min="0" max="255" value="100"><b id="tmpv">100</b></label>
  <button class="p" id="rw">پاداش +۱۰</button>
  <button class="d" id="pn">تنبیه −۱۰</button>
  <button class="d" id="off">خاموش کردن و ذخیره</button>
</header>
<div class="wrap">
  <div id="verdict" class="verdict v-warn">در حال جمع‌آوری داده…</div>

  <div class="grid">
    <div class="card"><div class="lbl">نورون زنده</div><div class="val" id="alive">—</div><div class="sub" id="deadn"></div></div>
    <div class="card"><div class="lbl">نرخ فایر</div><div class="val" id="fhz">—</div><div class="sub">هرتز بر نورون</div></div>
    <div class="card"><div class="lbl">کل مانا</div><div class="val" id="mana">—</div><div class="sub" id="transit"></div></div>
    <div class="card"><div class="lbl">رویداد</div><div class="val" id="evs">—</div><div class="sub" id="evrate"></div></div>
    <div class="card"><div class="lbl">خطای تابع</div><div class="val" id="flt">—</div><div class="sub">نورون به خواب رفته</div></div>
  </div>

  <div class="panel">
    <h3>وضعیت جمعیت</h3>
    <div class="states">
      <div class="st ok">سالم <b id="s_h">—</b></div>
      <div class="st warn">ایگنور <b id="s_i">—</b></div>
      <div class="st bad">اسپم <b id="s_s">—</b></div>
      <div class="st dim">خواب زمستانی <b id="s_d">—</b></div>
      <div class="st dim">خوابیده (خطا) <b id="s_a">—</b></div>
      <div class="st bad">مرده <b id="s_x">—</b></div>
    </div>
  </div>

  <div class="row">
    <div class="panel"><h3>استخر مانای لوب‌ها</h3>
      <div id="pools"></div></div>
    <div class="panel"><h3>نرخ فایر در طول زمان</h3><canvas id="c1"></canvas>
      <h3 style="margin-top:12px">پرشدگی استخر</h3><canvas id="c2"></canvas></div>
  </div>

  <div class="panel"><h3>جمعیت زنده در طول زمان</h3><canvas id="c3"></canvas></div>
  <div class="panel chatpanel">
    <h3>گفتگو و نمره‌دهی</h3>
    <div class="chatstats">
      <span>کلمات تولیدشده <b id="wt">۰</b></span>
      <span>نمره‌داده‌شده <b id="ws">۰</b></span>
      <span>میانگین نمره <b id="wa">۰</b></span>
      <label class="auto"><input type="checkbox" id="autoscroll" checked> دنبال کردن</label>
    </div>

    <div id="stream" class="stream"></div>

    <div class="composer">
      <input id="msg" type="text" placeholder="چیزی به مدل بگو…" autocomplete="off">
      <button class="p" id="send">بگو</button>
    </div>
    <div class="hint">
      روی هر کلمه کلیک کن تا نمره بدهی · نمره‌ی سریع با کلیک راست: <b>+۱۰</b>
    </div>
  </div>

  <div id="pop" class="pop">
    <div class="popw" id="popw">—</div>
    <div class="popr">
      <button data-s="-1000">−۱۰۰۰</button>
      <button data-s="-100">−۱۰۰</button>
      <button data-s="-10">−۱۰</button>
      <button data-s="-1">−۱</button>
      <button data-s="1">۱</button>
      <button data-s="3">۳</button>
      <button data-s="5">۵</button>
      <button data-s="10">۱۰</button>
    </div>
    <div class="popr2">
      <input id="custom" type="number" placeholder="نمره‌ی دلخواه" step="1">
      <button class="p" id="capply">اعمال</button>
      <button id="pclose">بستن</button>
    </div>
  </div>

  <div class="panel"><h3>جریان خام بیت‌ها</h3><div class="out" id="out">…</div></div>
</div>
<script>
const LOBE=['لوب ورودی','لوب مرکزی','لوب پایانی'];
const fa=n=>n.toLocaleString('fa-IR');
function spark(id,data,color,lo,hi){
  const c=document.getElementById(id),d=c.getContext('2d'),W=c.width=c.clientWidth*2,H=c.height=220;
  d.clearRect(0,0,W,H); if(!data||data.length<2)return;
  let mn=lo!==undefined?lo:Math.min(...data), mx=hi!==undefined?hi:Math.max(...data);
  if(mx-mn<1e-9)mx=mn+1;
  d.strokeStyle='#1c2634';d.lineWidth=1;
  for(let i=0;i<=4;i++){const y=H*i/4;d.beginPath();d.moveTo(0,y);d.lineTo(W,y);d.stroke();}
  d.beginPath();d.strokeStyle=color;d.lineWidth=3;
  data.forEach((v,i)=>{const x=W*i/(data.length-1),y=H-(v-mn)/(mx-mn)*H*0.9-H*0.05;
    i?d.lineTo(x,y):d.moveTo(x,y);});
  d.stroke();
  d.fillStyle='#5d7086';d.font='22px sans-serif';d.textAlign='right';
  d.fillText(mx.toFixed(2),W-6,26); d.fillText(mn.toFixed(2),W-6,H-8);
}
async function tick(){
  let s; try{ s=await (await fetch('/stats')).json(); }catch(e){ return; }
  document.getElementById('vt').textContent='زمان مجازی '+s.vt.toFixed(1)+'s';
  document.getElementById('alive').textContent=fa(s.alive);
  document.getElementById('deadn').textContent='مرده: '+fa(s.dead);
  document.getElementById('fhz').textContent=s.fire_hz.toFixed(2);
  document.getElementById('mana').textContent=fa(Math.round(s.mana));
  document.getElementById('transit').textContent='در ترانزیت: '+fa(Math.round(s.transit));
  document.getElementById('evs').textContent=fa(s.events);
  document.getElementById('evrate').textContent=fa(Math.round(s.evrate))+' بر ثانیه';
  document.getElementById('flt').textContent=fa(s.faults);
  document.getElementById('s_h').textContent=fa(s.healthy);
  document.getElementById('s_i').textContent=fa(s.ignoring);
  document.getElementById('s_s').textContent=fa(s.spamming);
  document.getElementById('s_d').textContent=fa(s.dormant);
  document.getElementById('s_a').textContent=fa(s.asleep);
  document.getElementById('s_x').textContent=fa(s.dead);
  let ph='';
  for(let i=0;i<3;i++){
    const p=s.pool[i],t=s.ptgt[i],pc=Math.max(0,Math.min(150,p/t*100));
    const col=pc<15?'#ff6b6b':pc<45?'#ffc857':'#3ddc84';
    ph+=`<div class="dim" style="font-size:11px">${LOBE[i]} — ${fa(s.alive_lobe[i])} نورون</div>
      <div class="bar"><i style="width:${Math.min(100,pc)}%;background:${col}"></i>
      <span><b>${pc.toFixed(0)}٪</b><b>${fa(Math.round(p))} / ${fa(Math.round(t))}</b></span></div>`;
  }
  document.getElementById('pools').innerHTML=ph;
  spark('c1',s.h_fire,'#4da3ff',0);
  spark('c2',s.h_pool,'#3ddc84',0);
  spark('c3',s.h_alive,'#c792ea');
  document.getElementById('out').textContent=s.out||'…';
  renderStream(s);
  document.getElementById('wt').textContent=fa(s.wtotal||0);
  document.getElementById('ws').textContent=fa(s.wscored||0);
  document.getElementById('wa').textContent=(s.wavg||0).toFixed(1);
  const v=document.getElementById('verdict');
  const alivePct=s.alive/(s.alive+s.dead)*100;
  if(s.vt<10){v.className='verdict v-warn';v.textContent='در حال جمع‌آوری داده…';}
  else if(alivePct<50){v.className='verdict v-bad';
    v.textContent='⚠ فروپاشی — بیش از نیمی از جمعیت مرده است. اقتصاد پایدار نیست.';}
  else if(s.fire_hz<0.02){v.className='verdict v-bad';
    v.textContent='⚠ انجماد — شبکه عملاً ساکت است. هزینه‌ی زنده‌ماندن جواب نداده.';}
  else if(s.fire_hz>60){v.className='verdict v-bad';
    v.textContent='⚠ انفجار — نرخ فایر مهارنشده. مانای در ترانزیت کافی نیست.';}
  else{v.className='verdict v-ok';
    v.textContent='✓ تعادل زنده — جمعیت پایدار، فعالیت مداوم، اقتصاد در حال کار.';}
}
// ---------- جریان گفتگو ----------
let lastKey='';
function renderStream(s){
  const el=document.getElementById('stream');
  const items=[];
  (s.chat||[]).forEach(m=>items.push({t:m.t,human:1,text:m.m,id:'c'+m.id}));
  (s.words||[]).forEach(w=>items.push({t:w.t,human:0,text:w.w,id:w.id,sc:w.s,done:w.d}));
  items.sort((a,b)=>a.t-b.t);
  const key=items.map(i=>i.id+':'+(i.sc||0)+(i.done||0)).join(',');
  if(key===lastKey)return;
  lastKey=key;
  const near=el.scrollHeight-el.scrollTop-el.clientHeight<80;
  let h='';
  for(const it of items){
    if(it.human){ h+=`<div class="hw">${esc(it.text)}</div>`; continue; }
    const cls=it.done?(it.sc>0?'w sc':(it.sc<0?'w sn':'w')):'w';
    const badge=it.done?`<span class="b">${it.sc>0?'+':''}${fa(it.sc)}</span>`:'';
    h+=`<span class="${cls}" data-id="${it.id}" data-w="${esc(it.text)}">${esc(it.text)}${badge}</span>`;
  }
  el.innerHTML=h||'<span class="dim">هنوز کلمه‌ای تولید نشده…</span>';
  if(near&&document.getElementById('autoscroll').checked) el.scrollTop=el.scrollHeight;
}
const esc=t=>t.replace(/[&<>"]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}[c]));

// ---------- نمره‌دهی ----------
let curId=null;
const pop=document.getElementById('pop');
document.getElementById('stream').addEventListener('click',e=>{
  const w=e.target.closest('.w'); if(!w)return;
  curId=w.dataset.id;
  document.getElementById('popw').textContent=w.dataset.w;
  const r=w.getBoundingClientRect();
  pop.classList.add('on');
  const pw=pop.offsetWidth,ph=pop.offsetHeight;
  let x=r.left+r.width/2-pw/2, y=r.bottom+8;
  if(y+ph>innerHeight-10) y=r.top-ph-8;
  pop.style.left=Math.max(10,Math.min(innerWidth-pw-10,x))+'px';
  pop.style.top=Math.max(10,y)+'px';
  document.getElementById('custom').value='';
});
document.getElementById('stream').addEventListener('contextmenu',e=>{
  const w=e.target.closest('.w'); if(!w)return;
  e.preventDefault(); score(w.dataset.id,10);
});
function score(id,v){
  fetch('/score?id='+id+'&s='+v).then(()=>{lastKey='';tick();});
  pop.classList.remove('on'); curId=null;
}
pop.querySelectorAll('.popr button').forEach(b=>
  b.onclick=()=>{ if(curId) score(curId,+b.dataset.s); });
document.getElementById('capply').onclick=()=>{
  const v=parseInt(document.getElementById('custom').value,10);
  if(curId&&!isNaN(v)) score(curId,v);};
document.getElementById('custom').onkeydown=e=>{
  if(e.key==='Enter') document.getElementById('capply').click();};
document.getElementById('pclose').onclick=()=>{pop.classList.remove('on');curId=null;};
document.addEventListener('click',e=>{
  if(!pop.contains(e.target)&&!e.target.closest('.w')) pop.classList.remove('on');});

// ---------- ارسال پیام ----------
function send(){
  const i=document.getElementById('msg'), t=i.value.trim();
  if(!t)return;
  fetch('/say?t='+encodeURIComponent(t)).then(()=>{i.value='';lastKey='';tick();});
}
document.getElementById('send').onclick=send;
document.getElementById('msg').onkeydown=e=>{if(e.key==='Enter')send();};

document.getElementById('pause').onclick=async e=>{
  const r=await (await fetch('/pause')).json();
  e.target.textContent=r.paused?'ادامه':'توقف';};
document.getElementById('spd').oninput=e=>{
  const m=[0,0.05,0.1,0.2,0.3,0.5,0.7,1,1.5,2,3,5,8,12,20,35,60,100,200,500,0];
  const i=+e.target.value, v=i>=20?0:m[i];
  document.getElementById('spdv').textContent=v===0?'بیشینه':v+'×';
  fetch('/speed?v='+Math.round(v*1000));};
document.getElementById('tmp').oninput=e=>{
  document.getElementById('tmpv').textContent=e.target.value;
  fetch('/temp?v='+e.target.value);};
document.getElementById('rw').onclick=()=>fetch('/reward?v=10000');
document.getElementById('pn').onclick=()=>fetch('/reward?v=-10000');
document.getElementById('off').onclick=async()=>{
  if(!confirm('مدل متوقف و حافظه‌ی همه‌ی نورون‌ها در brain.dat ذخیره شود؟'))return;
  await fetch('/shutdown'); alert('ذخیره شد: brain.dat');};
setInterval(tick,500); tick();
</script></body></html>)HTML";

static std::string json_stats() {
    std::lock_guard<std::mutex> lk(g_mx);
    Stats& s = g_stats;
    char buf[4096];
    std::string j = "{";
    snprintf(buf, sizeof buf,
        "\"vt\":%.3f,\"alive\":%lld,\"dead\":%lld,\"healthy\":%lld,\"ignoring\":%lld,"
        "\"spamming\":%lld,\"dormant\":%lld,\"asleep\":%lld,\"fire_hz\":%.4f,"
        "\"mana\":%.1f,\"transit\":%.1f,\"events\":%lld,\"evrate\":%.1f,\"faults\":%lld,",
        (double)s.vtime_us / SEC, (long long)s.alive, (long long)s.dead,
        (long long)s.healthy, (long long)s.ignoring, (long long)s.spamming,
        (long long)s.dormant, (long long)s.asleep, s.fire_hz,
        (double)s.total_mana / MANA, (double)s.transit / MANA,
        (long long)s.events, s.wall_s > 0 ? s.events / s.wall_s : 0.0,
        (long long)s.faults);
    j += buf;

    j += "\"pool\":[";
    for (int L = 0; L < N_LOBES; ++L) { snprintf(buf,sizeof buf,"%s%.1f",L?",":"",(double)s.pool[L]/MANA); j+=buf; }
    j += "],\"ptgt\":[";
    for (int L = 0; L < N_LOBES; ++L) { snprintf(buf,sizeof buf,"%s%.1f",L?",":"",(double)s.ptgt[L]/MANA); j+=buf; }
    j += "],\"alive_lobe\":[";
    for (int L = 0; L < N_LOBES; ++L) { snprintf(buf,sizeof buf,"%s%lld",L?",":"",(long long)s.alive_lobe[L]); j+=buf; }
    j += "],";

    auto arr = [&](const char* name, std::vector<double>& v, bool last) {
        j += "\""; j += name; j += "\":[";
        for (size_t i = 0; i < v.size(); ++i) { snprintf(buf,sizeof buf,"%s%.4f",i?",":"",v[i]); j+=buf; }
        j += last ? "]" : "],";
    };
    arr("h_fire", s.hist_fire, false);
    arr("h_pool", s.hist_pool, false);
    arr("h_alive", s.hist_alive, false);

    auto esc = [](const std::string& in) {
        std::string o;
        for (char c : in) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if ((unsigned char)c >= 32) o += c;
        }
        return o;
    };

    j += "\"out\":\"" + esc(s.out_text) + "\",";

    snprintf(buf, sizeof buf, "\"wtotal\":%lld,\"wscored\":%lld,\"wavg\":%.2f,",
             (long long)s.words_total, (long long)s.words_scored, s.avg_score);
    j += buf;

    j += "\"words\":[";
    for (size_t i = 0; i < s.words.size(); ++i) {
        const OutWord& w = s.words[i];
        snprintf(buf, sizeof buf, "%s{\"id\":%u,\"t\":%.1f,\"s\":%d,\"d\":%s,\"w\":\"",
                 i ? "," : "", w.id, w.t, w.score, w.scored ? "1" : "0");
        j += buf; j += esc(w.text); j += "\"}";
    }
    j += "],\"chat\":[";
    for (size_t i = 0; i < s.chat.size(); ++i) {
        const ChatMsg& m = s.chat[i];
        snprintf(buf, sizeof buf, "%s{\"id\":%u,\"t\":%.1f,\"h\":%s,\"m\":\"",
                 i ? "," : "", m.id, m.t, m.from_human ? "1" : "0");
        j += buf; j += esc(m.text); j += "\"}";
    }
    j += "]}";
    return j;
}

static int qparam(const std::string& req, const char* key, int def) {
    std::string k = std::string(key) + "=";
    size_t p = req.find(k);
    if (p == std::string::npos) return def;
    return atoi(req.c_str() + p + k.size());
}

static void http_server(int port) {
#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2,2), &wsa);
#endif
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) { perror("socket"); return; }
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);          // 0.0.0.0
    a.sin_port = htons((u16)port);
    if (bind(srv, (sockaddr*)&a, (socklen_t)sizeof a) != 0) {
        fprintf(stderr, "\n  [!] پورت %d اشغال است. با --port عدد دیگری بدهید.\n", port);
        fflush(stderr);
        close_sock(srv);
        g_running.store(false);
        return;
    }
    listen(srv, 32);
    g_server_up.store(true);
    printf("  داشبورد آماده است:  http://localhost:%d\n", port);
    printf("  ─────────────────────────────────────────────────\n");
    printf("  این پنجره را باز نگه دارید. بستن آن مغز را متوقف می‌کند.\n\n");
    fflush(stdout);

    while (g_running.load()) {
        sock_t c = accept(srv, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        char req[4096] = {0};
        int n = (int)recv(c, req, (int)sizeof(req) - 1, 0);
        if (n <= 0) { close_sock(c); continue; }
        std::string R(req, req + n);

        std::string body, ctype = "application/json; charset=utf-8";
        if (R.rfind("GET /stats", 0) == 0) {
            body = json_stats();
        } else if (R.rfind("GET /pause", 0) == 0) {
            bool p = !g_paused.load(); g_paused.store(p);
            body = std::string("{\"paused\":") + (p ? "true" : "false") + "}";
        } else if (R.rfind("GET /speed", 0) == 0) {
            g_speed.store(qparam(R, "v", 1000)); body = "{\"ok\":1}";
        } else if (R.rfind("GET /temp", 0) == 0) {
            B.temperature.store(std::max(0, std::min(255, qparam(R, "v", 100)))); body = "{\"ok\":1}";
        } else if (R.rfind("GET /score", 0) == 0) {
            int id = qparam(R, "id", 0), sc = qparam(R, "s", 0);
            if (id) device_score((u32)id, sc);
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /say", 0) == 0) {
            // متن به‌صورت URL-encoded در پارامتر t
            std::string t;
            size_t p = R.find("t=");
            if (p != std::string::npos) {
                size_t e = R.find_first_of(" &", p);
                std::string raw = R.substr(p + 2, e == std::string::npos ? std::string::npos : e - p - 2);
                for (size_t i = 0; i < raw.size(); ++i) {
                    if (raw[i] == '%' && i + 2 < raw.size()) {
                        int hi = raw[i+1], lo = raw[i+2];
                        auto hv = [](int c){ return c<='9'?c-'0':(c|32)-'a'+10; };
                        t += (char)((hv(hi) << 4) | hv(lo)); i += 2;
                    } else if (raw[i] == '+') t += ' ';
                    else t += raw[i];
                }
            }
            if (!t.empty()) device_say(t);
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /reward", 0) == 0) {
            g_reward_pending.fetch_add(qparam(R, "v", 0)); body = "{\"ok\":1}";
        } else if (R.rfind("GET /shutdown", 0) == 0) {
            g_shutdown_req.store(true);
            for (int i = 0; i < 100 && g_shutdown_req.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            body = "{\"saved\":1}";
        } else {
            body = PAGE; ctype = "text/html; charset=utf-8";
        }
        char hdr[512];
        int hl = snprintf(hdr, sizeof hdr,
            "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
            "Access-Control-Allow-Origin: *\r\nCache-Control: no-store\r\n"
            "Connection: close\r\n\r\n", ctype.c_str(), body.size());
        send(c, hdr, (int)hl, MSG_NOSIGNAL);
        send(c, body.data(), (int)body.size(), MSG_NOSIGNAL);
        close_sock(c);
    }
    close_sock(srv);
}

// ============================================================================
//  ۱۶. main
// ============================================================================

// کنسول ویندوز پیش‌فرض UTF-8 نیست → متن فارسی خراب نمایش داده می‌شود
static void console_utf8() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// باز کردن خودکار مرورگر روی داشبورد
static void open_browser(int port) {
    char url[64];
    snprintf(url, sizeof url, "http://localhost:%d", port);
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    char cmd[128]; snprintf(cmd, sizeof cmd, "open '%s' >/dev/null 2>&1 &", url);
    (void)system(cmd);
#else
    char cmd[128]; snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", url);
    (void)system(cmd);
#endif
}

int main(int argc, char** argv) {
    console_utf8();
    int  N = 5000, port = 8420, headless_s = 0;
    u64  seed = 12345;
    const char* loadf = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&]() -> const char* { return (i + 1 < argc) ? argv[++i] : "0"; };
        if      (a == "--neurons") N = atoi(nxt());
        else if (a == "--port")    port = atoi(nxt());
        else if (a == "--seed")    seed = (u64)atoll(nxt());
        else if (a == "--load")    loadf = nxt();
        else if (a == "--headless")headless_s = atoi(nxt());
        else if (a == "--speed")   g_speed.store(atoi(nxt()));
        else if (a == "--no-browser") g_open_browser = false;
    }

    printf("\n");
    printf("  ╔═══════════════════════════════════════════════╗\n");
    printf("  ║   سارینا — مرحله‌ی صفر                        ║\n");
    printf("  ║   یک مغز دیجیتال رویدادمحور                   ║\n");
    printf("  ╚═══════════════════════════════════════════════╝\n\n");

    if (loadf && load_brain(loadf)) {
        printf("  بارگذاری از چک‌پوینت: %s  (%zu نورون)\n", loadf, B.n.size());
    } else {
        build_brain(N, seed);
        int nn=0,nm=0,ng=0;
        for (auto& x : B.n) (x.kind==K_NORMAL?nn:x.kind==K_MEMORY?nm:ng)++;
        size_t edges = 0; for (auto& x : B.n) edges += x.out.size();
        printf("  نورون‌ها : %d  (عادی %d · حافظه‌ای %d · غول %d)\n", N, nn, nm, ng);
        printf("  یال‌ها   : %zu\n", edges);
        printf("  بذر     : %llu\n", (unsigned long long)seed);
    }
    printf("\n");

    std::thread srv;
    if (!headless_s) srv = std::thread(http_server, port);

    std::thread sim(sim_loop);

    // منتظر بالا آمدن سرور، سپس باز کردن مرورگر
    if (!headless_s) {
        for (int i = 0; i < 40 && g_running.load() && !g_server_up.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (g_server_up.load() && g_open_browser) open_browser(port);
    }

    if (headless_s) {
        // اجرای بی‌داشبورد برای سنجش — پایان قطعی در مرز رویداد (تکرارپذیر)
        g_speed.store(0);
        g_stop_at = B.now + (vtime)headless_s * SEC;
        auto t0 = std::chrono::steady_clock::now();
        printf("  %6s %8s %8s %7s %7s %7s %9s %8s\n",
               "زمان","زنده","سالم","ایگنور","اسپم","مرده","فایر/ثانیه","استخر٪");
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::lock_guard<std::mutex> lk(g_mx);
            Stats& s = g_stats;
            double pp = 0; for (int L=0;L<N_LOBES;++L) pp += (double)s.pool[L]/std::max<i64>(1,s.ptgt[L]);
            pp = pp / N_LOBES * 100;
            printf("  %5.1fs %8lld %8lld %7lld %7lld %7lld %9.2f %7.0f%%\n",
                   (double)s.vtime_us/SEC, (long long)s.alive, (long long)s.healthy,
                   (long long)s.ignoring, (long long)s.spamming, (long long)s.dead,
                   s.fire_hz, pp);
            fflush(stdout);
            if (std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count() > 600) break;
        }
        g_running.store(false);
        sim.join();
        printf("\n  اثرانگشت: vt=%lld fires=%lld signals=%lld events=%lld faults=%lld\n",
               (long long)B.now, (long long)B.c_fires, (long long)B.c_signals,
               (long long)B.c_events, (long long)B.c_faults);
        save_brain("brain.dat");
        printf("\n  ذخیره شد: brain.dat\n");
        return 0;
    }

    // گزارش زنده در کنسول — تا کاربر ببیند مغز کار می‌کند
    {
        int line = 0;
        while (g_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            std::lock_guard<std::mutex> lk(g_mx);
            Stats& s = g_stats;
            if (s.alive == 0) continue;
            if (line % 12 == 0) {
                printf("\n  %7s %9s %9s %8s %8s %11s\n",
                       "time", "alive", "healthy", "fire/s", "pool%", "events/s");
                printf("  %7s %9s %9s %8s %8s %11s\n",
                       "-------", "---------", "---------", "--------", "--------", "-----------");
            }
            double pp = 0;
            for (int L = 0; L < N_LOBES; ++L) pp += (double)s.pool[L] / std::max<i64>(1, s.ptgt[L]);
            pp = pp / N_LOBES * 100;
            printf("  %6.0fs %9lld %9lld %8.2f %7.0f%% %11.0f\n",
                   (double)s.vtime_us / SEC, (long long)s.alive, (long long)s.healthy,
                   s.fire_hz, pp, s.wall_s > 0 ? s.events / s.wall_s : 0.0);
            fflush(stdout);
            ++line;
        }
    }

    sim.join();
    g_running.store(false);
    if (srv.joinable()) srv.detach();
    printf("\n  متوقف شد.\n");
    return 0;
}
