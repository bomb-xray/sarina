// ============================================================================
//  smile — فاز ۲: یادگیری فارسی خودکار
//  smile — Phase 2 prototype
//
//  یک مغز دیجیتال رویدادمحور. تک‌فایل، بدون وابستگی.
//  هدف این مرحله: پاسخ به تنها سؤالی که با فکر کردن حل نمی‌شود —
//      «آیا اقتصاد مانا به تعادل زنده می‌رسد یا به انفجار/انجماد می‌افتد؟»
//
//  build:  g++ -O2 -std=c++17 -pthread smile.cpp -o smile
//  run:    ./smile [--neurons N] [--port P] [--seed S] [--load brain.dat]
//
//  مرجع: ARCHITECTURE.md  (پیش‌نویس ۰٫۳)
// ============================================================================

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <deque>
#include <condition_variable>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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

using i16 = int16_t;
using i32 = int32_t;
using i64 = int64_t;
using u8  = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

static constexpr u32 NO_NEURON = std::numeric_limits<u32>::max();

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
// --- پهنای باند حسی/حرکتی: ثابت و مستقل از اندازه‌ی مغز (بند ۲۶) ---
// مثل عصب بینایی که با بزرگ‌تر شدن قشر بینایی، تعداد رشته‌هایش عوض نمی‌شود.
// مغز بزرگ‌تر یعنی پردازش عمیق‌تر، نه دهان بزرگ‌تر یا گوش بیشتر.
static constexpr int MOUTH_COUNT = 14;            // نورون‌های متصل به خروجی واقعی
static constexpr int EAR_COUNT   = 48;            // نورون‌های گیرنده‌ی ورودی انسان
static constexpr int MIRROR_COUNT= 48;            // نورون‌های گیرنده‌ی آینه

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
    u8   is_mouth = 0;               // آیا به خروجی واقعی وصل است؟ (بند ۲۴)
    u8   is_ear   = 0;               // آیا ورودی بیرونی می‌گیرد؟ (بند ۲۶)
    i32  x = 0, y = 0;               // مختصات — برای سیم‌کشی محلی‌گرا (بند ۱۲٫۳)

    i64  mana     = 0;               // میلی‌مانا
    i64  cap      = 0;
    u16  credit   = 0;               // اعتبار فعالیت کوتاه‌مدت
    i16  plasticity = 0;             // اثر پاداش علّی بلندمدت: −۸۱۹۲..+۸۱۹۲
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
    std::vector<u32>   in_src;       // آخرین فرستنده‌ی هر خط — ردپای علّی پاداش
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
    u32   source;                     // فرستنده‌ی سیگنال؛ برای ردپای علّی
    u64   seq;                        // شکستن تساوی — ۳۲ بیت در اجرای دائم سرریز می‌کرد
};
// صف رویداد رادیکس یکنواخت. زمان شبیه‌سازی فقط رو به جلو می‌رود، پس
// priority_queue دودویی با O(log N) برای هر push/pop اتلاف بود (۲۶٫۵٪ CPU).
// radix heap کلید زمان را سرشکن O(1) نگه می‌دارد و در زمان مساوی seq را
// دقیقاً مثل صف قبلی مرتب می‌کند؛ بنابراین اثرانگشت اجرای قطعی عوض نمی‌شود.
class EventQueue {
    std::array<std::vector<Event>, 65> bucket_;
    u64 last_ = 0;
    size_t size_ = 0;

    static int slot(u64 key, u64 last) {
        u64 x = key ^ last;
        return x ? 64 - __builtin_clzll(x) : 0;
    }

    void prepare() {
        if (!bucket_[0].empty()) return;
        int b = 1;
        while (b < 65 && bucket_[b].empty()) ++b;
        if (b == 65) return;
        u64 next = std::numeric_limits<u64>::max();
        for (const Event& e : bucket_[b]) next = std::min(next, (u64)e.t);
        last_ = next;
        std::vector<Event> tmp;
        tmp.swap(bucket_[b]);
        for (Event& e : tmp) bucket_[slot((u64)e.t, last_)].push_back(std::move(e));
        // همه‌ی bucket صفر زمان یکسان دارند؛ back باید کمترین seq باشد.
        std::sort(bucket_[0].begin(), bucket_[0].end(),
                  [](const Event& a, const Event& b) { return a.seq > b.seq; });
    }

public:
    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }
    void push(const Event& e) {
        // همه‌ی تأخیرها مثبت‌اند؛ کلید عقب‌رونده یعنی نقض بنیادی زمان‌بند.
        if (e.t < 0 || (u64)e.t < last_) {
            fprintf(stderr, "fatal: non-monotonic event time (%lld < %llu)\n",
                    (long long)e.t, (unsigned long long)last_);
            std::abort();
        }
        bucket_[slot((u64)e.t, last_)].push_back(e);
        ++size_;
    }
    const Event& top() { prepare(); return bucket_[0].back(); }
    void pop() { prepare(); bucket_[0].pop_back(); --size_; }
    void reset(u64 last = 0) {
        for (auto& b : bucket_) b.clear();
        last_ = last; size_ = 0;
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
    vtime penalty_until = 0;      // تا این زمان درآمد پایه کم است (بند ۲۱)
    vtime boost_until   = 0;      // تا این زمان درآمد پایه بیشتر است
    i64   burn_recent   = 0;      // مانای سوخته‌ی اخیر — سنجه‌ی تقاضا
};

// --- دستگاه: کلمه‌ی خروجی که منتظر نمره است (بند ۱۱٫۲) ---
struct OutWord {
    u32         id;
    std::string text;
    double      t;                 // زمان مجازی تولد، ثانیه
    int         score = 0;         // آخرین نمره‌ی دستی (سازگاری رابط)
    i64         score_milli = 0;   // نمره‌ی نمایشی × ۱۰۰۰
    i64         auto_reward = 0;   // سیگنال خودکار اعمال‌شده
    i64         manual_reward = 0; // سیگنال دستی اعمال‌شده
    int         quality = 0;       // کیفیت نهایی ۰..۱۰۰
    int         spelling_quality = 0;
    int         dictionary_quality = 0;
    double      teacher_baseline = 0;
    double      teacher_advantage = 0;
    u8          teacher_mode = 0;  // ۱ املایی · ۲ دیکشنری · ۳ ترکیبی
    bool        exact = false;     // عضو دقیق واژه‌نامه
    bool        scored = false;
    bool        auto_scored = false;
    bool        manual_scored = false;
    std::vector<u32> trace;        // نورون‌های واقعاً در ساخت این واژه
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
    i64 words_auto = 0, words_manual = 0, words_exact = 0;
    i64 words_positive = 0, words_negative = 0, words_neutral = 0;
    i64 plasticity_positive = 0, plasticity_negative = 0;
    double plasticity_avg = 0;
    double avg_score = 0, avg_quality = 0, teacher_baseline = 0;
    double teacher_baseline_by_mode[4] = {0,0,0,0};
    i64 auto_reward_total = 0;
    i64 teacher_count[4] = {0,0,0,0};
    double teacher_quality_avg[4] = {0,0,0,0};
    double teacher_reward_avg[4] = {0,0,0,0};
};

struct Brain {
    std::vector<Neuron> n;
    LobePool            lp[N_LOBES];
    EventQueue          q;

    vtime now  = 0;
    u64   seq  = 0;
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
    struct OutBit { u8 bit; u32 source; vtime t; };
    std::vector<OutBit> out_bits;
    std::string         out_text;

    // --- دستگاه: بخش زبان ---
    std::string          cur_word;        // کلمه‌ی در حال ساخت
    std::vector<OutWord> words;           // کلمات کامل‌شده
    std::vector<ChatMsg> chat;
    u32                  next_word_id = 1;
    u32                  next_msg_id  = 1;
    i64                  words_total  = 0;
    i64                  words_scored = 0;
    i64                  words_auto   = 0;
    i64                  words_manual = 0;
    i64                  words_exact  = 0;
    i64                  words_positive = 0;
    i64                  words_negative = 0;
    i64                  words_neutral  = 0;
    i64                  auto_reward_total = 0;
    i64                  teacher_count[4] = {0,0,0,0};
    i64                  teacher_quality_sum[4] = {0,0,0,0};
    i64                  teacher_reward_sum[4] = {0,0,0,0};
    double               score_sum    = 0;
    double               quality_sum  = 0;
    double               teacher_baseline[4] = {0.0,45.0,45.0,45.0};
    std::vector<u32>     cur_trace;       // ردپای واژه‌ی در حال ساخت

    // --- دستگاه: بخش حواس (ورودی انسان → مغز) ---
    std::string     utf8_buf;             // بافر دنباله‌ی UTF-8
    int             utf8_need = 0;        // چند بایت ادامه لازم است
    i64             chars_ok = 0, chars_bad = 0;
    std::deque<u8>  in_queue;             // بیت‌های منتظر تزریق
    std::deque<u8>  mirror_queue;         // آینه: خروجی خودش با تأخیر
    vtime           next_inject = 0;

    inline void push(vtime t, u32 target, u8 type, u8 line = 0, u8 bit = 0,
                     u32 source = NO_NEURON) {
        q.push(Event{t, target, type, line, bit, source, seq++});
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
static std::atomic<int>  g_talkativeness{100};   // ۱۰..۴۰۰ ٪ — کنترل زنده‌ی پرحرفی

// --- معلم خودکار واژه (فاز ۲) ---
// ۰ خاموش · ۱ املایی/ngram · ۲ دیکشنری · ۳ ترکیبی
static std::atomic<int>  g_teacher_mode{3};
static std::atomic<int>  g_teacher_strength{0};  // پیش‌فرض فقط داوری؛ آموزش خودکار هنوز A/B را نبرده
static std::string       g_words_path = "persian_words.tsv";
static std::string       g_user_words_path = "my_words.tsv";

struct PendingFeedback {
    std::vector<u32> trace;
    i64 milli = 0;
};
static std::mutex                  g_feedback_mx;
static std::deque<PendingFeedback> g_feedback;

// --- موازی‌سازی (بند ۲۸) ---
static std::atomic<int>  g_threads{0};           // ۰ = خودکار (همه‌ی هسته‌ها)
static std::atomic<int>  g_cpu_percent{100};     // ۱۰..۱۰۰ ٪ سقف مصرف CPU
static std::atomic<double> g_cpu_measured{0.0};  // درصد واقعی کل پردازنده
static std::atomic<double> g_virtual_speed{0.0}; // ثانیه‌ی مجازی / ثانیه‌ی واقعی
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
    c.push_back(enc(OP_MUL, 7, 1, IM(40)));           // ورودی: وزن بالا
    c.push_back(enc(OP_MUL, 8, 2, IM(55)));           // تازگی: وزن بالاتر
    c.push_back(enc(OP_ADD, 7, 7, RG(8)));
    c.push_back(enc(OP_DIV, 8, 3, IM(18)));           // نویز: فقط تلنگر (۱/۳ قبل)
    c.push_back(enc(OP_ADD, 7, 7, RG(8)));
    c.push_back(enc(OP_DIV, 8, 5, IM(120)));          // بی‌قراری: کند
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
        nu.in_src.assign(KIND_LINES[kind], NO_NEURON);
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

    // --- انتخاب «گوش»: قیف ورودی (بند ۲۶) ---
    // تعداد ثابت، مستقل از اندازه‌ی مغز. نیمه‌ی الف از انسان می‌شنود،
    // نیمه‌ی ب آینه‌ی خود مدل را.
    {
        std::vector<u32> ears[2];
        for (auto& nu : B.n)
            if (nu.lobe == L_INPUT) ears[nu.half & 1].push_back(nu.id);
        const int want[2] = { EAR_COUNT, MIRROR_COUNT };
        for (int h = 0; h < 2; ++h) {
            size_t w = std::min<size_t>((size_t)want[h], ears[h].size());
            for (size_t k = 0; k < w; ++k) {
                size_t j = R.below((u32)ears[h].size());
                B.n[ears[h][j]].is_ear = 1;
                ears[h].erase(ears[h].begin() + j);
            }
        }
    }

    // --- انتخاب «دهان»: قیف خروجی (بند ۲۴) ---
    // حدود ۳٪ لوب پایانی به خروجی واقعی وصل می‌شود، با کف ۸ و سقف ۴۰.
    {
        std::vector<u32> outs;
        for (auto& nu : B.n) if (nu.lobe == L_OUTPUT) outs.push_back(nu.id);
        size_t want = std::min<size_t>((size_t)MOUTH_COUNT, outs.size());
        for (size_t k = 0; k < want && !outs.empty(); ++k) {
            size_t j = R.below((u32)outs.size());
            B.n[outs[j]].is_mouth = 1;
            outs.erase(outs.begin() + j);
        }
    }

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
    B.q.reset(0);
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

static void apply_reward(i64 milli) {          // پاداش/تنبیه از دستگاه (بند ۵٫۳)
    if (milli == 0) return;
    // فشرده‌سازی لگاریتمی + مقیاس نسبت به اندازه‌ی لوب (بند ۱۱٫۲)
    //
    // اصلاح مرحله‌ی ۰: پیش‌تر ضریب ثابت ۳ بود، یعنی نمره‌ی −۱۰ فقط ۷ مانا
    // می‌شد — در برابر استخر ۱۱٬۸۸۰ مانا حدود ۰٫۰۶٪، که درآمد پایه در
    // یک‌دهم ثانیه جبرانش می‌کرد. حالا اثر نمره با اندازه‌ی لوب مقیاس
    // می‌گیرد: یک نمره‌ی ±۱۰ حدود ۱۵٪ استخر لوب پایانی را جابه‌جا می‌کند.
    double s   = (double)milli;
    double mag = std::log(1.0 + std::fabs(s) / MANA) / std::log(11.0);   // ±۱۰ → ۱٫۰
    double c   = (s >= 0 ? 1.0 : -1.0) * mag * (double)B.lp[L_OUTPUT].target * 0.20;
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

    // تنبیه فقط استخر را خالی نمی‌کند؛ درآمد پایه را هم موقتاً قطع می‌کند،
    // وگرنه در کسری از ثانیه جبران می‌شود و بی‌اثر می‌ماند.
    if (v > 0) {
        // پاداش هم ماندگار است: لوب‌های عقب برای مدتی درآمد بیشتری می‌گیرند
        vtime bd = (vtime)(8.0 * SEC * mag);
        B.lp[L_INPUT].boost_until   = std::max(B.lp[L_INPUT].boost_until,   B.now + bd);
        B.lp[L_CENTRAL].boost_until = std::max(B.lp[L_CENTRAL].boost_until, B.now + bd);
    }
    if (v < 0) {
        vtime dur = (vtime)(3.0 * SEC * mag);
        B.lp[L_OUTPUT].penalty_until  = std::max(B.lp[L_OUTPUT].penalty_until,  B.now + dur);
        B.lp[L_CENTRAL].penalty_until = std::max(B.lp[L_CENTRAL].penalty_until, B.now + dur / 2);
    }

    // کف حیاتی: تنبیه نمی‌تواند استخر را زیر ۵٪ هدف ببرد (بند ۱۱٫۲)
    for (int L = 0; L < N_LOBES; ++L) {
        i64 floor_ = B.lp[L].target * 12 / 100;
        if (B.lp[L].pool < floor_) B.lp[L].pool = floor_;
    }
}

// سهم محلی پاداش: استخر لوب همچنان اقتصاد کلان است، اما «اعتبار» فقط به
// نورون‌هایی می‌رسد که رسید علّی همان واژه را دارند. این حلقه‌ی گمشده‌ی
// نسخه‌ی قبل بود؛ در آن نسخه هر نورون هم‌لوب از یک کلمه‌ی خوب سود می‌برد.
static void apply_pending_feedback() {
    std::deque<PendingFeedback> pending;
    {
        std::lock_guard<std::mutex> lk(g_feedback_mx);
        pending.swap(g_feedback);
    }
    for (const PendingFeedback& fb : pending) {
        double mag = std::log(1.0 + std::fabs((double)fb.milli) / MANA) / std::log(11.0);
        int sign = fb.milli >= 0 ? 1 : -1;
        for (u32 id : fb.trace) {
            if (id >= B.n.size() || B.n[id].state == S_DEAD) continue;
            Neuron& nu = B.n[id];
            double lobe_weight = nu.lobe == L_OUTPUT ? 1.0 : (nu.lobe == L_CENTRAL ? 0.65 : 0.40);
            i64 delta = (i64)std::llround(6000.0 * mag * lobe_weight) * sign;
            i64 next = (i64)nu.credit + delta;
            nu.credit = (u16)std::max<i64>(0, std::min<i64>(65535, next));
            // credit فقط اقتصاد را عوض می‌کرد و در آزمون A/B هیچ رفتار خروجی
            // را تغییر نداد. plasticity مستقیماً نرخ/گیت فایر همان مسیر علّی
            // را تغییر می‌دهد و آهسته باقی می‌ماند.
            i64 pdelta = (i64)std::llround(2500.0 * mag * lobe_weight) * sign;
            i64 pnext = (i64)nu.plasticity + pdelta;
            nu.plasticity = (i16)std::max<i64>(-8192, std::min<i64>(8192, pnext));
        }
    }
}

// ============================================================================
//  ۱۱. حلقه‌ی رویداد
// ============================================================================

static void deliver(u32 dst, u8 line, u8 bit, u32 source = NO_NEURON) {
    Neuron& t = B.n[dst];
    if (t.state == S_DEAD) return;
    if (line >= t.lines()) return;
    if (bit) t.in_bits |=  (1ull << line);
    else     t.in_bits &= ~(1ull << line);
    t.in_at[line]  = B.now;
    t.in_src[line] = source;
    t.last_input   = B.now;
    if (t.state == S_DORMANT) t.state = S_HEALTHY;          // بیدار شدن (بند ۵٫۴)
}

// ارزیاب جداگانه‌ی تک‌نخ حذف شد؛ neuron_eval_mt با Worker صفر همان مسیر را
// برای یک نخ هم اجرا می‌کند و داشتن دو نسخه باعث واگرایی باگ‌ها می‌شد.

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
            i64 spill = 0;
            for (int L = 0; L < N_LOBES; ++L) {
                i64 part = amt * B.lp[L].cap_sum / tot;
                // لوبِ در دوره‌ی بدهی، مانای بازگشتی نمی‌گیرد.
                // بدون این، تنبیه در ۵ ثانیه شسته می‌شد و استخر از ۵٪
                // مستقیم به ۱۰۰٪ می‌پرید — دقیقاً همان چیزی که دیده شد.
                // مانای بازگشتیِ لوبِ تنبیه‌شده نه به خودش می‌رسد و نه به
                // لوب دیگری منتقل می‌شود — واقعاً می‌سوزد. اگر منتقل می‌شد،
                // تنبیه فقط ثروت را جابه‌جا می‌کرد نه اینکه درد بسازد.
                if (B.now < B.lp[L].penalty_until) { B.treasury += part / 2; part /= 2; }
                i64 room = B.lp[L].target - B.lp[L].pool;
                if (room < 0) room = 0;
                i64 give = std::min(part, room);
                B.lp[L].pool += give;
                spill        += part - give;      // مازاد لوب‌های سیر
            }
            // مازاد به گرسنه‌ترین لوب می‌رود، نه به خزانه.
            // پیش‌تر به خزانه می‌رفت و هرگز خرج نمی‌شد — یک نشت واقعی
            // که کل اقتصاد را خشک می‌کرد (لوب ورودی/مرکزی روی ۱٪ گیر می‌کردند).
            while (spill > 0) {
                int hungriest = 0; double worst = 2.0;
                for (int L = 0; L < N_LOBES; ++L) {
                    if (B.now < B.lp[L].penalty_until) continue;   // در دوره‌ی بدهی
                    double f = (double)B.lp[L].pool / std::max<i64>(1, B.lp[L].target);
                    if (f < worst) { worst = f; hungriest = L; }
                }
                if (worst >= 1.0) { B.treasury += spill; break; }
                i64 room = B.lp[hungriest].target - B.lp[hungriest].pool;
                i64 give = std::min(spill, room > 0 ? room : 0);
                if (give <= 0) { B.treasury += spill; break; }
                B.lp[hungriest].pool += give;
                spill -= give;
            }
        } else B.treasury += amt;
    }

    // --- درآمد پایه + نشت خودتنظیم (بند ۵٫۳) ---
    // اصلاح مرحله‌ی ۰: درآمد باید با ظرفیت وزن بخورد، نه سرانه‌ی تخت.
    // با درآمد تخت، نورون حافظه‌ای (هزینه ۰٫۸/s) و غول (۲٫۴/s) از روز اول
    // ورشکسته‌ی ساختاری‌اند و می‌میرند — که در اولین اجرا دقیقاً رخ داد.
    for (int L = 0; L < N_LOBES; ++L) {
        LobePool& P = B.lp[L];
        // --- درآمد پایه = نان شب، نه ثروت (اصلاح بند ۲۲) ---
        // سقف معیشت: درآمد پایه فقط تا ۲۵٪ هدف کار می‌کند. بالاتر از آن،
        // تنها پاداش می‌تواند استخر را پر کند. بدون این، شبکه بدون هیچ
        // کاری در ۳۵ ثانیه به ۱۰۰٪ می‌رسید و نمره‌دهی بی‌معنا می‌شد.
        // لوب ورودی ته زنجیره‌ی نشت است، پس سقف معیشتش بالاتر (بند ۵٫۳)
        const double SUBSIST = (L == L_INPUT) ? 0.45 : 0.25;
        double f = (double)P.pool / std::max<i64>(1, P.target);

        i64 income = 0;
        if (f < SUBSIST) {
            // درآمد به تقاضای واقعی گره خورده، نه فقط ظرفیت.
            // لوبی که خرج نمی‌کند (مثل لوب پایانیِ کم‌فایر) انباشت نمی‌کند.
            i64 capbased = P.cap_sum * BASE_INCOME / (20 * MANA) * SYS_TICK / SEC;
            i64 demand   = P.burn_recent * 2;
            income = std::min(capbased, std::max(demand, capbased / 4));
            income = (i64)(income * (1.0 + 2.0 * (SUBSIST - f) / SUBSIST));
        }

        if (B.now < P.penalty_until)    income /= 3;   // دوره‌ی بدهی پس از تنبیه
        else if (B.now < P.boost_until) income *= 3;   // دوره‌ی رونق پس از پاداش
        if (P.emergency) income *= 3;

        i64 ceil_ = (i64)(P.target * SUBSIST);
        i64 room  = ceil_ - P.pool;
        if (room < 0) room = 0;
        P.pool += std::min(income, room);

        P.burn_recent = P.burn_recent * 3 / 4;         // محو تدریجی تقاضا
    }

    // --- سرریز به عقب (بند ۵٫۳ + اصلاح ۲۲) ---
    //
    // پیش‌تر شرط `pool > target` بود، ولی استخر دقیقاً روی سقف بسته می‌شد،
    // پس هرگز سرریز نمی‌کرد: لوب پایانی یک مخزن بن‌بست بود که روی ۱۰۰٪
    // می‌ماند در حالی که لوب‌های عقب گرسنه بودند.
    //
    // حالا هر استخری که از ۷۰٪ هدف بگذرد، مازادش را رو به عقب می‌فرستد.
    // مانا در سیستم می‌چرخد به‌جای اینکه در انتها تلنبار شود.
    for (int L = N_LOBES - 1; L > 0; --L) {
        LobePool& hi = B.lp[L];
        LobePool& lo = B.lp[L - 1];
        // لوبی که خودش تنبیه شده، نه سرریز می‌کند نه سرریز می‌گیرد —
        // وگرنه تنبیه در دو ثانیه شسته می‌شود.
        i64 spill_from = hi.target * 2 / 5;
        if (B.now < hi.penalty_until) continue;
        if (hi.pool > spill_from && B.now >= lo.penalty_until) {
            i64 over = hi.pool - spill_from;
            i64 give = over / 2;                 // یک‌سوم در هر تیک — جریان نرم
            i64 room = lo.target - lo.pool;
            if (room < 0) room = 0;
            give = std::min(give, room);
            if (give > 0) { hi.pool -= give; lo.pool += give; }
        }
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
    // ابتدا اعتبار مسیرهای علّی، سپس اقتصاد کلان لوب‌ها.
    apply_pending_feedback();
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
// ---------------------------------------------------------------------------
//  الفبای خروجی — ۶ بیت = ۶۴ نماد (بند ۱۱٫۱)
//
//  فقط ۳۲ حرف فارسی + آ + چهار نشانه نگه داشته شده‌اند. شکل‌های عربیِ
//  همزه‌دار، ارقام و نشانه‌های کم‌کاربرد حذف شدند؛ خانه‌های اضافه با فاصله
//  و حروف پرتکرار فارسی پر شده‌اند. تکرار یک نماد فقط وزن کدک است و حرف
//  تازه‌ای به زبان اضافه نمی‌کند.
// ---------------------------------------------------------------------------
static constexpr int PERSIAN_LETTER_COUNT = 33;
static const char* PERSIAN_ALPHABET[64] = {
    " ", "ا", "ب", "پ", "ت", "ث", "ج", "چ",
    "ح", "خ", "د", "ذ", "ر", "ز", "ژ", "س",
    "ش", "ص", "ض", "ط", "ظ", "ع", "غ", "ف",
    "ق", "ک", "گ", "ل", "م", "ن", "و", "ه",
    "ی", "آ", "،", ".", "؟", "!", " ", " ",
    " ", " ", " ", " ", " ", "ا", "ا", "ا",
    "ن", "ن", "ر", "ر", "ی", "ی", "ی", "م",
    "م", "و", "و", "د", "ه", "ه", "ب", "ت"
};

// نگاشت معکوس برای ورودی انسان
static int persian_index(const std::string& ch) {
    for (int i = 0; i < 64; ++i)
        if (ch == PERSIAN_ALPHABET[i]) return i;
    return 0;   // ناشناخته → فاصله
}

// ---------------------------------------------------------------------------
//  معلم خودکار فارسی — فایل داده بیرون از کد می‌ماند
//
//  فرمت v2: واژه<TAB>فراوانی<TAB>وضعیت<TAB>یادداشت، UTF-8.
//  پایه‌ی curated با my_words.tsv ادغام می‌شود و از verifiedها مدل
//  دو/سه‌حرفی ساخته می‌شود. بنابراین داوری فقط صفر/یک نیست و خروجیِ نزدیک
//  به فارسی نیز سیگنال ضعیف می‌گیرد؛ وگرنه مغز در ابتدای کار فقط تنبیه
//  می‌بیند و سکوت را یاد می‌گیرد.
// ---------------------------------------------------------------------------
static constexpr int TEACH_V = 65;       // ۰..۶۳ نمادها؛ ۶۴ مرز واژه
static constexpr int TEACH_BOUNDARY = 64;

struct TeacherLexicon {
    std::unordered_map<std::string,u32> freq;
    std::unordered_set<std::string> verified;
    std::unordered_set<std::string> suggested;
    std::unordered_set<std::string> blocked;
    std::vector<u64> bigram = std::vector<u64>((size_t)TEACH_V * TEACH_V, 0);
    std::vector<u64> trigram = std::vector<u64>((size_t)TEACH_V * TEACH_V * TEACH_V, 0);
    u64 max_freq = 1;
    bool loaded = false;
    std::string loaded_path;
    std::string error;
};
static TeacherLexicon g_lexicon;

static void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size())
        s.replace(p, from.size(), to);
}

// فقط نرمال‌سازی‌هایی که کانال ۶ بیتی مغز قادر به بازنمایی آن‌هاست.
static std::string normalize_word(std::string s) {
    replace_all(s, "ي", "ی"); replace_all(s, "ى", "ی");
    replace_all(s, "ك", "ک"); replace_all(s, "ۀ", "ه");
    replace_all(s, "ة", "ه"); replace_all(s, "إ", "ا"); replace_all(s, "أ", "ا");
    replace_all(s, "ؤ", "و"); replace_all(s, "ئ", "ی"); replace_all(s, "ء", "");
    replace_all(s, "ٱ", "ا"); replace_all(s, "ـ", "");
    replace_all(s, "‌", "");  replace_all(s, "ٔ", "");
    const char* marks[] = {"َ","ِ","ُ","ّ","ْ","ً","ٍ","ٌ"};
    for (const char* m : marks) replace_all(s, m, "");
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.erase(s.begin());
    while (!s.empty() && (s.back()  == ' ' || s.back()  == '\t' || s.back()  == '\r')) s.pop_back();
    return s;
}

static bool word_symbols(const std::string& word, std::vector<int>& out) {
    out.clear();
    for (size_t i = 0; i < word.size();) {
        unsigned char c = (unsigned char)word[i];
        size_t n = (c < 0x80) ? 1 : ((c & 0xE0) == 0xC0 ? 2 : ((c & 0xF0) == 0xE0 ? 3 : 4));
        if (i + n > word.size()) return false;
        std::string ch = word.substr(i, n);
        int k = persian_index(ch);
        // فقط شناسه‌های canonical حروف فارسی؛ تکرارهای وزنی و نشانه‌ها واژه نیستند.
        if (k <= 0 || k > PERSIAN_LETTER_COUNT) return false;
        out.push_back(k);
        i += n;
    }
    return !out.empty();
}

static bool load_teacher_data(const std::string& path) {
    g_lexicon.freq.clear(); g_lexicon.verified.clear();
    g_lexicon.suggested.clear(); g_lexicon.blocked.clear();
    g_lexicon.freq.reserve(120000); g_lexicon.verified.reserve(120000);
    std::fill(g_lexicon.bigram.begin(), g_lexicon.bigram.end(), 0);
    std::fill(g_lexicon.trigram.begin(), g_lexicon.trigram.end(), 0);
    g_lexicon.max_freq = 1;

    std::vector<int> syms;
    auto load_file = [&](const std::string& file, bool required) -> bool {
        std::ifstream f(file, std::ios::binary);
        if (!f) {
            if (required) g_lexicon.error = "فایل داده پیدا نشد: " + file;
            return !required;
        }
        std::string line;
        while (std::getline(f, line)) {
            if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
                (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
                line.erase(0, 3);
            if (line.empty() || line[0] == '#') continue;
            std::vector<std::string> col;
            size_t p = 0;
            while (true) {
                size_t q = line.find('\t', p);
                col.push_back(line.substr(p, q == std::string::npos ? q : q - p));
                if (q == std::string::npos) break;
                p = q + 1;
            }
            if (col.empty()) continue;
            std::string word = normalize_word(col[0]);
            if (!word_symbols(word, syms) || syms.size() > 12) continue;
            u64 n = 1;
            if (col.size() > 1) {
                try { n = std::max<u64>(1, std::stoull(col[1])); } catch (...) { n = 1; }
            }
            std::string status = col.size() > 2 ? col[2] : "verified";
            for (char& c : status) if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
            bool is_block = status == "blocked" || status == "block" || status == "مسدود";
            bool is_suggest = status == "suggested" || status == "suggest" || status == "پیشنهاد";
            bool is_verify = status.empty() || status == "verified" || status == "verify" || status == "تایید";
            if (is_block) {
                g_lexicon.blocked.insert(word); g_lexicon.verified.erase(word);
                g_lexicon.suggested.erase(word); g_lexicon.freq.erase(word); continue;
            }
            if (is_suggest) {
                g_lexicon.suggested.insert(word); g_lexicon.verified.erase(word);
                g_lexicon.freq.erase(word); continue;
            }
            if (!is_verify) continue;
            g_lexicon.blocked.erase(word); g_lexicon.suggested.erase(word);
            g_lexicon.verified.insert(word);
            g_lexicon.freq[word] = (u32)std::min<u64>(n, UINT32_MAX);
        }
        g_lexicon.loaded_path += (g_lexicon.loaded_path.empty() ? "" : " + ") + file;
        return true;
    };

    g_lexicon.loaded_path.clear();
    if (!load_file(path, true)) return false;
    std::string user = g_user_words_path;
    std::ifstream probe(user);
    if (!probe) {
        size_t slash = path.find_last_of("/\\");
        if (slash != std::string::npos) user = path.substr(0, slash + 1) + g_user_words_path;
    }
    load_file(user, false);

    // فقط واژه‌های verified مدل املایی را می‌سازند. frequency وزن است، نه
    // مدرک اعتبار؛ عضویت را واژه‌نامه‌ی curated تعیین می‌کند.
    std::vector<int> seq;
    for (const auto& kv : g_lexicon.freq) {
        if (!g_lexicon.verified.count(kv.first) || !word_symbols(kv.first, syms)) continue;
        g_lexicon.max_freq = std::max<u64>(g_lexicon.max_freq, kv.second);
        seq.clear(); seq.push_back(TEACH_BOUNDARY);
        seq.insert(seq.end(), syms.begin(), syms.end()); seq.push_back(TEACH_BOUNDARY);
        u64 wt = std::max<u64>(1, (u64)std::sqrt((double)kv.second));
        for (size_t i = 1; i < seq.size(); ++i)
            g_lexicon.bigram[(size_t)seq[i-1] * TEACH_V + seq[i]] += wt;
        for (size_t i = 2; i < seq.size(); ++i)
            g_lexicon.trigram[((size_t)seq[i-2] * TEACH_V + seq[i-1]) * TEACH_V + seq[i]] += wt;
    }
    g_lexicon.loaded = !g_lexicon.verified.empty();
    g_lexicon.error = g_lexicon.loaded ? "" : "واژه‌ی تاییدشده‌ای در فایل داده نیست";
    return g_lexicon.loaded;
}

struct JudgeResult {
    int quality = 0;
    int spelling = 0;
    int dictionary = 0;
    bool exact = false;
    std::string normalized;
};

static JudgeResult judge_word(const std::string& raw, int mode) {
    JudgeResult R;
    if (!g_lexicon.loaded) return R;
    std::string w = normalize_word(raw);

    // نشانه‌ی پایانی مجاز است، ولی نشانه وسط واژه کیفیت را خراب می‌کند.
    int punctuation = 0;
    const char* punct[] = {".","،","؟","!",":","؛","-","«","»"};
    bool changed = true;
    while (changed && !w.empty()) {
        changed = false;
        for (const char* p : punct) {
            std::string q(p);
            if (w.size() >= q.size() && w.compare(w.size()-q.size(), q.size(), q) == 0) {
                w.erase(w.size()-q.size()); punctuation++; changed = true; break;
            }
        }
    }
    R.normalized = w;
    std::vector<int> a;
    if (!word_symbols(w, a)) return R;

    auto it = g_lexicon.freq.find(w);
    // frequency دیگر رأی اعتبار نیست. فقط عضویت curated/verified حق پاداش
    // دیکشنری می‌دهد؛ suggested بی‌اثر و blocked حتی از پایه حذف است.
    R.exact = it != g_lexicon.freq.end() && g_lexicon.verified.count(w) != 0;

    std::vector<int> q; q.reserve(a.size()+2);
    q.push_back(TEACH_BOUNDARY); q.insert(q.end(), a.begin(), a.end()); q.push_back(TEACH_BOUNDARY);
    double logsum = 0;
    int transitions = 0;
    for (size_t i = 1; i < q.size(); ++i) {
        int x = q[i-1], y = q[i];
        u64 row2 = 0;
        for (int k = 0; k < TEACH_V; ++k)
            row2 += g_lexicon.bigram[(size_t)x * TEACH_V + k];
        double p2 = (g_lexicon.bigram[(size_t)x * TEACH_V + y] + 1.0) /
                    (row2 + (double)TEACH_V);
        double p = p2;
        if (i >= 2) {
            int z = q[i-2];
            size_t base = ((size_t)z * TEACH_V + x) * TEACH_V;
            u64 row3 = 0;
            for (int k = 0; k < TEACH_V; ++k) row3 += g_lexicon.trigram[base+k];
            double p3 = (g_lexicon.trigram[base+y] + 1.0) /
                        (row3 + (double)TEACH_V);
            p = 0.75 * p3 + 0.25 * p2;
        }
        logsum += std::log(std::max(1e-12, p)); transitions++;
    }
    double avglog = transitions ? logsum / transitions : -8.0;
    int spelling = (int)std::llround(std::max(0.0, std::min(100.0, (avglog + 6.2) * 100.0 / 4.5)));
    int lexical = 0;
    if (R.exact) {
        double fscore = std::log1p((double)it->second) / std::log1p((double)g_lexicon.max_freq);
        lexical = (int)std::llround(50.0 + 50.0 * fscore);
    }

    R.spelling = spelling;
    R.dictionary = lexical;
    if      (mode == 1) R.quality = spelling;
    else if (mode == 2) R.quality = lexical;
    else                R.quality = (int)std::llround(0.55 * spelling + 0.45 * lexical);

    // بستن راه تقلب: «مممم»، نشانه‌باران و تکرار یک واژه پاداش نمی‌گیرند.
    int run = 1, maxrun = 1;
    std::unordered_set<int> distinct(a.begin(), a.end());
    for (size_t i = 1; i < a.size(); ++i) {
        run = (a[i] == a[i-1]) ? run + 1 : 1;
        maxrun = std::max(maxrun, run);
    }
    if (maxrun >= 3) R.quality -= 25 * (maxrun - 2);
    if (a.size() >= 2 && distinct.size() == 1) R.quality -= 20;
    if (a.size() == 1 && w != "و") R.quality = std::min(R.quality, 20);
    R.quality -= punctuation * 4;
    int repeats = 0;
    for (auto itw = B.words.rbegin(); itw != B.words.rend() && repeats < 4; ++itw)
        if (normalize_word(itw->text) == w) repeats++;
    R.quality -= repeats * 18;
    R.quality = std::max(0, std::min(100, R.quality));
    return R;
}

static void queue_feedback(const std::vector<u32>& trace, i64 milli) {
    if (!milli || trace.empty()) return;
    std::lock_guard<std::mutex> lk(g_feedback_mx);
    g_feedback.push_back(PendingFeedback{trace, milli});
}

// ردگیری عقب‌گرد محلی: فقط ورودی‌هایی که در ۵۰ms پیش از فایر رسیده‌اند.
// این backprop نیست؛ یک رسید محلی از «چه کسی به چه کسی سیگنال داد» است.
static void collect_causal_trace(u32 root, vtime at, std::vector<u32>& dst) {
    if (root == NO_NEURON || root >= B.n.size()) return;
    struct Node { u32 id; vtime at; u8 depth; };
    std::deque<Node> todo;
    std::unordered_set<u32> global(dst.begin(), dst.end());
    std::unordered_set<u32> local;
    auto add = [&](u32 id, vtime t, u8 d) {
        if (id == NO_NEURON || id >= B.n.size() || global.size() >= 768) return;
        // در هر بیت مسیر تازه را دوباره می‌پیماییم، ولی شناسه در ردپای واژه
        // فقط یک بار ذخیره می‌شود. یک نورون دهان ممکن است چند ثانیه بعد با
        // ورودی‌های کاملاً متفاوت حرف بعدی را بسازد.
        if (!local.insert(id).second) return;
        if (global.insert(id).second) dst.push_back(id);
        todo.push_back(Node{id,t,d});
    };
    add(root, at, 0);
    while (!todo.empty() && global.size() < 768) {
        Node x = todo.front(); todo.pop_front();
        if (x.depth >= 7) continue;
        const Neuron& nu = B.n[x.id];
        for (size_t i = 0; i < nu.in_at.size(); ++i) {
            vtime t = nu.in_at[i];
            if (t >= 0 && t <= x.at && x.at - t <= 50 * MS)
                add(nu.in_src[i], t, (u8)(x.depth + 1));
        }
    }
}

// بستن کلمه‌ی جاری و فرستادنش برای نمره‌دهی
static void device_close_word() {
    if (B.cur_word.empty()) { B.cur_trace.clear(); return; }
    std::lock_guard<std::mutex> lk(g_mx);

    OutWord w;
    w.id = B.next_word_id++;
    w.text = B.cur_word + " ";           // فاصله به کلمه می‌چسبد (بند ۱۱٫۱)
    w.t = (double)B.now / SEC;
    w.trace.swap(B.cur_trace);

    int mode = g_teacher_mode.load(std::memory_order_relaxed);
    if (mode > 0 && g_lexicon.loaded) {
        JudgeResult J = judge_word(B.cur_word, mode);
        w.quality = J.quality;
        w.spelling_quality = J.spelling;
        w.dictionary_quality = J.dictionary;
        w.teacher_mode = (u8)mode;
        w.exact = J.exact;
        B.quality_sum += J.quality;
        if (J.exact) B.words_exact++;

        // مزیت نسبت به خط پایه‌ی متحرک: حتی پیش از ساخت اولین واژه‌ی کامل،
        // الگوهای «کمتر بد» پاداش می‌گیرند. میانگین‌گیری جلوی تورم مانا را می‌گیرد.
        double advantage = 0.0;
        double baseline_before = B.teacher_baseline[mode];
        if (B.teacher_count[mode] == 0) {
            // اولین نمونه‌ی هر نوع معلم فقط خط پایه‌ی خودش را می‌سازد؛ مقیاس
            // دیکشنری با املایی فرق دارد و نباید baseline همدیگر را آلوده کنند.
            baseline_before = J.quality;
            B.teacher_baseline[mode] = J.quality;
        } else {
            advantage = J.quality - baseline_before;
            B.teacher_baseline[mode] = 0.92 * baseline_before + 0.08 * J.quality;
        }
        if (J.exact && J.quality >= 40) advantage = std::max(5.0, advantage);
        advantage = std::max(-40.0, std::min(40.0, advantage));
        int strength = g_teacher_strength.load(std::memory_order_relaxed);
        i64 reward = (i64)std::llround(advantage * strength / 8.0);
        reward = std::max<i64>(-500, std::min<i64>(500, reward));

        w.teacher_baseline = baseline_before;
        w.teacher_advantage = advantage;
        w.auto_reward = reward;
        w.score_milli = reward;
        w.auto_scored = w.scored = true;
        B.words_auto++; B.words_scored++;
        B.score_sum += (double)reward / MANA;
        B.auto_reward_total += reward;
        B.teacher_count[mode]++;
        B.teacher_quality_sum[mode] += J.quality;
        B.teacher_reward_sum[mode] += reward;
        if (reward > 0) B.words_positive++;
        else if (reward < 0) B.words_negative++;
        else B.words_neutral++;
        // فشار اصلی خودکار روی اعتبار همان مسیر است. فقط یک‌هشتم به استخر
        // کل لوب می‌رود تا موج اولیه‌ی خطا، راه‌حل «برای همیشه ساکت شو» نسازد.
        g_reward_pending.fetch_add(reward / 8);
        queue_feedback(w.trace, reward);
    }

    B.words.push_back(std::move(w));
    B.words_total++;
    if (B.words.size() > 250) B.words.erase(B.words.begin());
    B.cur_word.clear();
}

static void device_decode() {
    while (B.out_bits.size() >= 6) {
        u8 sym = 0;
        for (int i = 0; i < 6; ++i) {
            const Brain::OutBit& ob = B.out_bits[i];
            sym = (u8)((sym << 1) | ob.bit);
            collect_causal_trace(ob.source, ob.t, B.cur_trace);
        }
        B.out_bits.erase(B.out_bits.begin(), B.out_bits.begin() + 6);

        // آینه: هرچه گفت با تأخیر به نیمه‌ی ب لوب ورودی برمی‌گردد (بند ۴)
        for (int i = 5; i >= 0; --i) B.mirror_queue.push_back((u8)((sym >> i) & 1));

        const char* g = PERSIAN_ALPHABET[sym & 63];
        B.chars_ok++;

        if (g[0] == ' ') {                       // فاصله → پایان کلمه
            device_close_word();
            B.out_text += " ";
        } else {
            B.cur_word += g;
            B.out_text += g;
            int letters = 0;                     // شمارش حرف، نه بایت
            for (size_t k = 0; k < B.cur_word.size(); ++k)
                if ((B.cur_word[k] & 0xC0) != 0x80) ++letters;
            if (letters >= 12) device_close_word();
        }
    }
    if (B.out_text.size() > 900) B.out_text.erase(0, B.out_text.size() - 900);
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

    // متن فارسی → نماد ۶ بیتی (معکوس همان الفبا)
    size_t i = 0;
    while (i < text.size()) {
        size_t len = 1;
        unsigned char c = (unsigned char)text[i];
        if      ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > text.size()) break;
        int idx = persian_index(text.substr(i, len));
        for (int b = 5; b >= 0; --b) B.in_queue.push_back((u8)((idx >> b) & 1));
        i += len;
    }
    for (int b = 5; b >= 0; --b) B.in_queue.push_back(0);   // فاصله‌ی پایانی
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
        // فقط به «گوش‌ها» تزریق می‌شود — مجموعه‌ای ثابت و از پیش تعیین‌شده،
        // نه هر نورونی که در پیمایش زودتر بیاید (بند ۲۶).
        for (auto& nu : B.n) {
            if (!nu.is_ear || nu.lobe != L_INPUT || nu.half != half) continue;
            if (nu.state == S_DEAD) continue;
            int nl = nu.lines();
            deliver(nu.id, (u8)(nl - 2), b0);
            deliver(nu.id, (u8)(nl - 1), b1);
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

        // نمره‌ی دستی روی معلم خودکار نوشته نمی‌شود؛ یک تصحیح مستقل و بسیار
        // قوی‌تر است. اگر انسان نمره‌ی خودش را عوض کرد فقط اختلاف اعمال می‌شود.
        i64 next = (i64)score * MANA;
        i64 delta = next - w.manual_reward;
        B.score_sum -= (double)w.score_milli / MANA;
        if (!w.scored) B.words_scored++;
        if (!w.manual_scored) { B.words_manual++; w.manual_scored = true; }
        w.score = score;
        w.manual_reward = next;
        w.score_milli = next;
        w.scored = true;
        B.score_sum += score;
        g_reward_pending.fetch_add(delta);
        queue_feedback(w.trace, delta);
        return;
    }
}

// ============================================================================
//  ۱۳. چک‌پوینت  —  brain.dat  (بند ۱۲٫۸)
// ============================================================================

static bool save_brain(const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    const char magic[8] = {'S','M','I','L','E','0','0','5'};
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
        fwrite(&nu.is_mouth, 1, 1, f); fwrite(&nu.is_ear, 1, 1, f);
        fwrite(&nu.x, 4, 1, f); fwrite(&nu.y, 4, 1, f);
        fwrite(&nu.mana, 8, 1, f); fwrite(&nu.cap, 8, 1, f);
        fwrite(&nu.credit, 2, 1, f);
        fwrite(&nu.plasticity, 2, 1, f);
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
    const char current_magic[8] = {'S','M','I','L','E','0','0','5'};
    const char v4_magic[8]      = {'S','M','I','L','E','0','0','4'};
    if (fread(magic,1,8,f) != 8 ||
        (memcmp(magic,current_magic,8) && memcmp(magic,v4_magic,8))) {
        fclose(f); return false;
    }
    bool has_plasticity = memcmp(magic,current_magic,8) == 0;
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
        fread(&nu.is_mouth,1,1,f); fread(&nu.is_ear,1,1,f);
        fread(&nu.x,4,1,f); fread(&nu.y,4,1,f);
        fread(&nu.mana,8,1,f); fread(&nu.cap,8,1,f);
        fread(&nu.credit,2,1,f);
        if (has_plasticity) fread(&nu.plasticity,2,1,f); else nu.plasticity = 0;
        fread(&nu.last_fire,8,1,f); fread(&nu.last_input,8,1,f);
        fread(&nu.prog,4,1,f);
        fread(&nu.rng.s,8,1,f);
        fread(&nu.in_bits,8,1,f);
        u32 msz=0; fread(&msz,4,1,f); nu.mem.resize(msz);
        if (msz) fread(nu.mem.data(),1,msz,f);
        u32 esz=0; fread(&esz,4,1,f); nu.out.resize(esz);
        for (u32 k=0;k<esz;++k){ fread(&nu.out[k].dst,4,1,f); fread(&nu.out[k].line,1,1,f); fread(&nu.out[k].delay,8,1,f); }
        nu.in_at.assign(KIND_LINES[nu.kind], -1);
        nu.in_src.assign(KIND_LINES[nu.kind], NO_NEURON);
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
    B.q.reset((u64)B.now);
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
        if (nu.plasticity > 0) s.plasticity_positive++;
        else if (nu.plasticity < 0) s.plasticity_negative++;
        s.plasticity_avg += nu.plasticity;
        s.total_mana += nu.mana;
    }
    if (s.alive) s.plasticity_avg /= s.alive;
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

    // device_score از نخ HTTP می‌آید؛ کپی واژه‌ها و آمار باید با همان قفل باشد.
    std::lock_guard<std::mutex> lk(g_mx);
    // ۴۰ کلمه‌ی آخر برای نمره‌دهی
    size_t wstart = B.words.size() > 40 ? B.words.size() - 40 : 0;
    s.words.assign(B.words.begin() + wstart, B.words.end());
    size_t cstart = B.chat.size() > 40 ? B.chat.size() - 40 : 0;
    s.chat.assign(B.chat.begin() + cstart, B.chat.end());
    s.words_total  = B.words_total;
    s.words_scored = B.words_scored;
    s.words_auto   = B.words_auto;
    s.words_manual = B.words_manual;
    s.words_exact  = B.words_exact;
    s.words_positive = B.words_positive;
    s.words_negative = B.words_negative;
    s.words_neutral  = B.words_neutral;
    s.auto_reward_total = B.auto_reward_total;
    s.avg_score    = B.words_scored ? B.score_sum / B.words_scored : 0.0;
    s.avg_quality  = B.words_auto ? B.quality_sum / B.words_auto : 0.0;
    int current_teacher = std::max(0, std::min(3, g_teacher_mode.load()));
    s.teacher_baseline = current_teacher ? B.teacher_baseline[current_teacher] : 0.0;
    for (int m = 1; m <= 3; ++m) {
        s.teacher_baseline_by_mode[m] = B.teacher_baseline[m];
        s.teacher_count[m] = B.teacher_count[m];
        s.teacher_quality_avg[m] = B.teacher_count[m]
            ? (double)B.teacher_quality_sum[m] / B.teacher_count[m] : 0.0;
        s.teacher_reward_avg[m] = B.teacher_count[m]
            ? (double)B.teacher_reward_sum[m] / B.teacher_count[m] / MANA : 0.0;
    }

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

// ---------------------------------------------------------------------------
//  موتور پنجره‌ی موازی (بند ۲۸)
//
//  رویدادهای EV_EVAL داخل یک پنجره‌ی ۱ms کاملاً مستقل‌اند (بند ۱۲٫۱):
//  سیگنالشان زودتر از t+1ms به هیچ‌جا نمی‌رسد. پس می‌توان همه را همزمان
//  اجرا کرد. هر نخ خروجی‌هایش را در بافر محلی جمع می‌کند و در پایان
//  پنجره — به ترتیب قطعی — در صف اصلی ادغام می‌شود، تا تکرارپذیری
//  حفظ شود (بند ۱۲٫۹).
// ---------------------------------------------------------------------------
struct Worker {
    std::vector<Event> out;          // رویدادهای تولیدشده
    i64 fires = 0, signals = 0, faults = 0;
    i64 burn[N_LOBES] = {0,0,0};
    i64 pool_draw[N_LOBES] = {0,0,0};
    std::vector<std::pair<vtime,i64>> transit;
    std::vector<Brain::OutBit> out_bits;
    char pad[64];                    // جلوگیری از false sharing
};
static std::vector<Worker> g_workers(256);   // اندازه‌ی ثابت: هرگز resize نمی‌شود

static int effective_threads() {
    int t = g_threads.load();
    if (t <= 0) t = (int)std::thread::hardware_concurrency();
    if (t <= 0) t = 1;
    return std::max(1, std::min(256, t));
}

// زمان CPU کل فرایند (جمع همه‌ی نخ‌ها)، برای عدد واقعی مشابه Task Manager.
// سنجه‌ی قبلی فقط نسبت خواب نخ هماهنگ‌کننده بود و می‌توانست ۱۰۰٪ نشان دهد
// در حالی که کل فرایند روی یک CPU چند‌هسته‌ای ۳۵٪ مصرف داشت.
static double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME create_t{}, exit_t{}, kernel_t{}, user_t{};
    if (!GetProcessTimes(GetCurrentProcess(), &create_t, &exit_t, &kernel_t, &user_t)) return 0;
    ULARGE_INTEGER k{}, u{};
    k.LowPart = kernel_t.dwLowDateTime; k.HighPart = kernel_t.dwHighDateTime;
    u.LowPart = user_t.dwLowDateTime;   u.HighPart = user_t.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) * 1e-7;
#else
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) return 0;
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
#endif
}

// اجرای یک نورون در حالت موازی — بدون دست زدن به حالت سراسری مشترک
static void neuron_eval_mt(u32 id, Worker& W, vtime now) {
    Neuron& nu = B.n[id];
    if (nu.state == S_DEAD) return;

    const vtime dt = now - nu.last_eval;
    nu.last_eval = now;

    i64 upkeep = nu.cap * UPKEEP_PCT / 1000 * dt / SEC;
    if (nu.state == S_DORMANT) upkeep /= 10;
    nu.mana -= upkeep;
    if (upkeep > 0) { W.transit.push_back({now + TRANSIT_TIME, upkeep}); }

    // برداشت از استخر — فقط درخواست ثبت می‌شود، تسویه در پایان پنجره
    if (nu.mana < nu.cap) {
        i64 want = nu.cap - nu.mana;
        i64 share = 15 + std::min<i64>(85, (i64)nu.credit / 180);
        i64 ask = want * share / 100;
        if (ask > 0) { W.pool_draw[nu.lobe] += ask; nu.mana += ask; nu.last_income = now; }
    }

    { i64 decay = (i64)nu.credit * dt / (5 * SEC);
      nu.credit = (u16)((i64)nu.credit > decay ? (i64)nu.credit - decay : 0); }
    { i64 decay = std::llabs((i64)nu.plasticity) * dt / (3600 * SEC);
      if (nu.plasticity > 0) nu.plasticity = (i16)std::max<i64>(0, (i64)nu.plasticity - decay);
      else if (nu.plasticity < 0) nu.plasticity = (i16)std::min<i64>(0, (i64)nu.plasticity + decay); }

    bool shielded = (now < B.lp[nu.lobe].penalty_until + 5 * SEC);
    if (nu.mana <= 0) {
        nu.mana = 0;
        if (!shielded && nu.state != S_SPAM && now - nu.last_income > STARVE_TIME) {
            nu.state = S_SPAM; nu.dcredit = DEATH_CREDIT; nu.spam_until = now + SPAM_TIME;
        }
    } else if (nu.mana * 100 < nu.cap * IGNORE_PCT) {
        if (nu.state == S_HEALTHY || nu.state == S_DORMANT) nu.state = S_IGNORE;
    } else if (nu.state == S_IGNORE) nu.state = S_HEALTHY;

    if (nu.state == S_HEALTHY && nu.last_input >= 0 && now - nu.last_input > DORMANT_TIME
        && nu.last_fire >= 0 && now - nu.last_fire > DORMANT_TIME) nu.state = S_DORMANT;

    vtime cadence = KIND_CADENCE[nu.kind];
    auto emit = [&](vtime t, u32 tgt, u8 ty, u8 ln, u8 bt) {
        W.out.push_back(Event{t, tgt, ty, ln, bt, id, 0});
    };

    if (nu.state == S_SPAM) {
        if (now >= nu.spam_until || nu.dcredit <= 0) { nu.state = S_DEAD; return; }
        int nl = nu.lines();
        u64 mask = nu.rng.next() & ((nl >= 64) ? ~0ull : ((1ull << nl) - 1));
        i64 cost = FIRE_STARTUP + FIRE_PER_LINE * __builtin_popcountll(mask);
        if (cost > nu.dcredit) nu.dcredit = 0;
        else {
            nu.dcredit -= cost;
            u64 bits = nu.rng.next();
            for (auto& e : nu.out)
                if (e.line < 64 && ((mask >> (e.line % nl)) & 1)) {
                    emit(now + e.delay, e.dst, EV_SIGNAL, e.line, (u8)((bits >> (e.line & 63)) & 1));
                    W.signals++;
                }
        }
        emit(now + cadence, id, EV_EVAL, 0, 0);
        return;
    }
    if (nu.state == S_IGNORE) { emit(now + cadence, id, EV_EVAL, 0, 0); return; }
    if (nu.state == S_ASLEEP) { emit(now + cadence * 4, id, EV_EVAL, 0, 0); return; }
    if (nu.state == S_DORMANT) cadence *= 10;
    // مسیر پاداش‌گرفته زودتر فرصت ارزیابی می‌گیرد؛ مسیر تنبیه‌شده کندتر.
    if (nu.plasticity > 0)
        cadence = std::max<vtime>(KIND_CADENCE[nu.kind] / 2,
                  cadence * (16384 - nu.plasticity) / 16384);
    else if (nu.plasticity < 0)
        cadence = cadence * (8192 - nu.plasticity) / 8192;

    VmResult res = vm_run(nu, B);
    if (res.fault) { nu.faults++; W.faults++; nu.state = S_ASLEEP;
                     emit(now + cadence * 4, id, EV_EVAL, 0, 0); return; }
    if (res.sleep) { emit(now + cadence * 3, id, EV_EVAL, 0, 0); return; }

    vtime refr = REFRACTORY;
    if (nu.is_mouth) {
        int t = g_talkativeness.load(std::memory_order_relaxed);
        refr = REFRACTORY * 15 * 100 / std::max(10, t);
    }
    if (nu.plasticity > 0) refr = std::max<vtime>(REFRACTORY / 2,
                                      refr * (16384 - nu.plasticity) / 16384);
    else if (nu.plasticity < 0) refr = refr * (8192 - nu.plasticity) / 8192;
    bool refractory = (nu.last_fire >= 0 && now - nu.last_fire < refr);
    // plasticity منفی تلاش فایر را نیز با احتمال قطعیِ بذردار سرکوب می‌کند.
    if (res.fired && nu.plasticity < 0) {
        u32 keep = (u32)std::max(0, 8192 + (int)nu.plasticity);
        if (nu.rng.below(8192) >= keep) res.fired = false;
    }

    if (res.fired && res.mask && !refractory) {
        int nl = nu.lines();
        i64 cost = FIRE_STARTUP + FIRE_PER_LINE * __builtin_popcountll(res.mask);
        if (nu.mana >= cost) {
            nu.mana -= cost;
            W.transit.push_back({now + TRANSIT_TIME, cost});
            W.burn[nu.lobe] += cost;
            nu.last_fire = now; nu.fires++; W.fires++;
            { i64 nc = (i64)nu.credit + cost / 16;
              nu.credit = (u16)std::min<i64>(nc, 65535); }
            for (auto& e : nu.out) {
                int li = e.line % nl;
                if ((res.mask >> li) & 1) {
                    emit(now + e.delay, e.dst, EV_SIGNAL, e.line, (u8)((res.bits >> (li & 63)) & 1));
                    W.signals++;
                }
            }
            if (nu.lobe == L_OUTPUT && nu.is_mouth) {
                int l0 = nl - 2, l1 = nl - 1;
                if (((res.mask >> l0) & 1) || ((res.mask >> l1) & 1)) {
                    W.out_bits.push_back(Brain::OutBit{(u8)((res.bits >> l0) & 1), id, now});
                    W.out_bits.push_back(Brain::OutBit{(u8)((res.bits >> l1) & 1), id, now});
                }
            }
        }
        nu.in_bits = 0;
    }
    emit(now + cadence / 2 + (vtime)nu.rng.below((u32)(cadence / 2)), id, EV_EVAL, 0, 0);
}

// ---------------------------------------------------------------------------
//  استخر نخ ماندگار (بند ۲۸٫۱)
//  ساخت نخ در هر پنجره‌ی ۱ms فاجعه است (~۲۰µs ساخت در برابر کار ناچیز).
//  نخ‌ها یک بار ساخته می‌شوند و با چرخش سبک منتظر کار می‌مانند.
// ---------------------------------------------------------------------------
namespace pool {
    std::vector<std::thread>  threads;
    std::atomic<int>          generation{0};
    std::atomic<int>          done{0};
    std::atomic<bool>         quit{false};
    const std::vector<u32>*   job = nullptr;
    vtime                     job_now = 0;
    int                       nthr = 0;
    std::mutex                mx;
    std::condition_variable   cv, cv_done;
}

static void pool_worker(int idx) {
    int seen = 0;
    while (true) {
        {   // خواب واقعی تا رسیدن کار — چرخش، CPU را حتی بی‌کار می‌سوزاند
            std::unique_lock<std::mutex> lk(pool::mx);
            pool::cv.wait(lk, [&]{ return pool::quit.load() ||
                                          pool::generation.load() != seen; });
            if (pool::quit.load()) return;
            seen = pool::generation.load();
        }
        const std::vector<u32>& ev = *pool::job;
        int nt = pool::nthr;
        size_t chunk = (ev.size() + nt - 1) / nt;
        size_t a = (size_t)idx * chunk, b = std::min(ev.size(), a + chunk);
        for (size_t k = a; k < b; ++k)
            neuron_eval_mt(ev[k], g_workers[idx], pool::job_now);
        if (pool::done.fetch_add(1, std::memory_order_release) + 1 >= pool::nthr - 1) {
            std::lock_guard<std::mutex> lk(pool::mx);
            pool::cv_done.notify_all();
        }
    }
}

static void pool_stop() {
    if (pool::threads.empty()) return;
    { std::lock_guard<std::mutex> lk(pool::mx);
      pool::quit.store(true, std::memory_order_release);
      pool::cv.notify_all(); }
    for (auto& t : pool::threads) if (t.joinable()) t.join();
    pool::threads.clear();
    pool::quit.store(false, std::memory_order_release);
}

static void pool_start(int nthr) {
    if ((int)pool::threads.size() == nthr - 1) return;
    pool_stop();
    pool::generation.store(0); pool::done.store(0);
    pool::nthr = nthr;
    for (int i = 1; i < nthr; ++i) pool::threads.emplace_back(pool_worker, i);
}

static void pool_run(int nthr, const std::vector<u32>& evals, vtime now) {
    pool_start(nthr);
    { std::lock_guard<std::mutex> lk(pool::mx);
      pool::job = &evals; pool::job_now = now; pool::nthr = nthr;
      pool::done.store(0, std::memory_order_release);
      pool::generation.fetch_add(1, std::memory_order_release);
      pool::cv.notify_all(); }

    // نخ اصلی هم سهم خودش را انجام می‌دهد
    size_t chunk = (evals.size() + nthr - 1) / nthr;
    size_t b = std::min(evals.size(), chunk);
    for (size_t k = 0; k < b; ++k) neuron_eval_mt(evals[k], g_workers[0], now);

    if (nthr > 1) {
        std::unique_lock<std::mutex> lk(pool::mx);
        pool::cv_done.wait(lk, [&]{ return pool::done.load() >= nthr - 1; });
    }
}

static void sim_loop() {
    using clk = std::chrono::steady_clock;
    auto  t0  = clk::now();
    vtime v0  = B.now;
    vtime next_snap = B.now;
    vtime next_dec  = B.now;
    auto cpu_sample_wall = clk::now();
    double cpu_sample_proc = process_cpu_seconds();
    vtime cpu_sample_vtime = B.now;
    int death_sweep = 0;

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
            pool_stop();
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

        auto win_t0 = clk::now();

        // ---------------- پنجره‌ی موازی (بند ۲۸) ----------------
        // همه‌ی رویدادهای بازه‌ی [t, t+WINDOW) را یکجا برمی‌داریم.
        // چون حداقل تأخیر یال ۱ms است، هیچ‌کدام نمی‌توانند روی
        // یکدیگر اثر بگذارند — پس اجرای موازی امن است (بند ۱۲٫۱).
        const vtime WINDOW = EDGE_MIN;
        const vtime w_end  = e.t + WINDOW;

        static std::vector<u32> evals;
        static std::vector<Event> others;
        evals.clear(); others.clear();

        while (!B.q.empty() && B.q.top().t < w_end) {
            Event ev = B.q.top(); B.q.pop();
            B.now = ev.t; B.c_events++;
            if (ev.type == EV_EVAL) evals.push_back(ev.target);
            else others.push_back(ev);
        }
        B.now = w_end - 1;

        // سیگنال‌ها و سیستم‌تیک ترتیبی‌اند (ارزان و باید مرتب باشند)
        for (auto& ev : others) {
            if (ev.type == EV_SIGNAL) deliver(ev.target, ev.line, ev.bit, ev.source);
            else if (ev.type == EV_SYS) system_tick();
        }

        // --- اجرای موازی نورون‌ها ---
        int nthr = effective_threads();
        // g_workers باید پیش از ساخت نخ‌ها تثبیت شود، وگرنه resize
        // بعدی اشاره‌گرهای نخ‌های در حال اجرا را باطل می‌کند.

        for (int i = 0; i < nthr; ++i) {
            Worker& W = g_workers[i];
            W.out.clear(); W.transit.clear(); W.out_bits.clear();
            W.fires = W.signals = W.faults = 0;
            for (int L = 0; L < N_LOBES; ++L) { W.burn[L] = 0; W.pool_draw[L] = 0; }
        }

        if (!evals.empty()) {
            if (nthr == 1 || evals.size() < 256) {
                for (u32 id : evals) neuron_eval_mt(id, g_workers[0], B.now);
            } else {
                pool_run(nthr, evals, B.now);
            }
        }

        // --- ادغام قطعی: ترتیب نخ‌ها ثابت است، پس تکرارپذیر می‌ماند ---
        for (int i = 0; i < nthr; ++i) {
            Worker& W = g_workers[i];
            for (auto& ev : W.out) B.push(ev.t, ev.target, ev.type, ev.line, ev.bit, ev.source);
            for (auto& tr : W.transit) { B.transit.push_back(tr); B.transit_total += tr.second; }
            for (const auto& b : W.out_bits) B.out_bits.push_back(b);
            B.c_fires += W.fires; B.c_signals += W.signals; B.c_faults += W.faults;
            for (int L = 0; L < N_LOBES; ++L) {
                B.lp[L].burn_recent += W.burn[L];
                B.lp[L].pool = std::max<i64>(0, B.lp[L].pool - W.pool_draw[L]);
            }
        }
        // مرتب‌سازی لازم نیست: همه‌ی ورودی‌های یک پنجره مهر زمانی یکسان
        // دارند و پنجره‌ها به ترتیب پیش می‌روند، پس صف ذاتاً مرتب می‌ماند.
        // (پیش‌تر std::sort روی ~۵۰۰ هزار عنصر در هر پنجره اجرا می‌شد
        //  که کل موتور را از کار می‌انداخت.)
        // ثبت مرگ‌ها: پیمایش کل جمعیت گران است؛ فقط هر ۵۰ پنجره یک بار.
        if (++death_sweep >= 50) {
            death_sweep = 0;
            for (auto& nu : B.n)
                if (nu.state == S_DEAD && nu.cap > 0) {
                    B.lp[nu.lobe].alive--; B.lp[nu.lobe].cap_sum -= nu.cap;
                    B.lp[nu.lobe].deaths_window++; nu.cap = 0;
                }
        }
        // رمزگشایی در همان پنجره انجام می‌شود تا in_src هنوز همان مسیر علّی
        // لحظه‌ی تولید باشد؛ تأخیر ۲۰۰ms ردپا را با سیگنال‌های بعدی می‌پوشاند.
        device_decode();
        if (B.out_bits.size() > 4096)
            B.out_bits.erase(B.out_bits.begin(), B.out_bits.begin() + 2048);

        device_inject();

        // --- محدودکننده‌ی مصرف CPU (بند ۲۸٫۲) ---
        // نسبت کار به استراحت را نگه می‌دارد: اگر سقف ۵۰٪ باشد،
        // به ازای هر واحد زمان کار، همان‌قدر می‌خوابد.
        {
            int lim = g_cpu_percent.load();
            if (lim < 100) {
                double busy = std::chrono::duration<double>(clk::now() - win_t0).count();
                double sleep_s = busy * (100.0 - lim) / std::max(1, lim);
                if (sleep_s > 0.00002) {
                    if (sleep_s > 0.05) sleep_s = 0.05;
                    std::this_thread::sleep_for(std::chrono::duration<double>(sleep_s));
                }
            }
        }

        // مصرف واقعی کل فرایند، نرمال‌شده به ۰..۱۰۰٪ کل پردازنده.
        auto cpu_now_wall = clk::now();
        double cpu_wall_dt = std::chrono::duration<double>(cpu_now_wall - cpu_sample_wall).count();
        if (cpu_wall_dt >= 0.5) {
            double cpu_now_proc = process_cpu_seconds();
            unsigned logical = std::max(1u, std::thread::hardware_concurrency());
            double pct = (cpu_now_proc - cpu_sample_proc) / cpu_wall_dt * 100.0 / logical;
            double vspeed = (double)(B.now - cpu_sample_vtime) / SEC / cpu_wall_dt;
            g_cpu_measured.store(std::max(0.0, std::min(100.0, pct)));
            g_virtual_speed.store(std::max(0.0, vspeed));
            cpu_sample_wall = cpu_now_wall;
            cpu_sample_proc = cpu_now_proc;
            cpu_sample_vtime = B.now;
        }

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
    pool_stop();     // بستن امن نخ‌ها، وگرنه terminate می‌دهد
}

// ============================================================================
//  ۱۵. داشبورد  —  وب‌سرور کوچک + HTML درون‌خطی (بند ۱۲٫۷)
// ============================================================================

static const char* PAGE = R"HTML(<!DOCTYPE html>
<html lang="fa" dir="rtl"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>smile — فاز ۲</title>
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
.teacherbar{display:flex;align-items:center;gap:14px;flex-wrap:wrap;background:#0c1119;
            border:1px solid #1b2534;border-radius:8px;padding:8px 11px;margin-bottom:10px;font-size:11px}
.teacherbar label{color:#75879e}.teacherbar b{color:#dfe7f0}.teacherbar .data{margin-right:auto;color:#65788f}
.teacherbar .data.ok{color:#3ddc84}.teacherbar .data.bad{color:#ff6b6b}
.teacher-types{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-bottom:10px}
.teacher-type{background:#0c1119;border:1px solid #1b2534;border-radius:8px;padding:9px 11px}
.teacher-type b{display:block;color:#9fc5f8;font-size:12px}.teacher-type span{display:block;color:#71849a;font-size:10px;line-height:1.6}
.teacher-type small{display:block;color:#dfe7f0;margin-top:5px;font-size:10px}
@media(max-width:800px){.teacher-types{grid-template-columns:1fr}}
.judge-note{font-size:11px;color:#71849a;margin:12px 0 6px}
.judge-wrap{overflow:auto;border:1px solid #1b2534;border-radius:8px;background:#080b10;max-height:360px}
.judge-table{width:100%;border-collapse:collapse;min-width:1050px;font-size:11px;font-variant-numeric:tabular-nums}
.judge-table th{position:sticky;top:0;background:#141d29;color:#8fa6c2;padding:7px 8px;border-bottom:1px solid #263448;white-space:nowrap;z-index:1}
.judge-table td{padding:6px 8px;border-bottom:1px solid #121b28;text-align:center;white-space:nowrap}
.judge-table tr:hover td{background:#101927}.judge-table .wordcell{font:13px monospace;color:#dfe7f0;direction:rtl}
.judge-table .pos{color:#3ddc84;font-weight:700}.judge-table .neg{color:#ff6b6b;font-weight:700}
.judge-table .zero{color:#75879e}.judge-table .exactyes{color:#4da3ff}.judge-table .manualscore{color:#c792ea}
select{background:#141d29;color:#dfe7f0;border:1px solid #2a3a52;border-radius:6px;
       padding:5px 8px;font-family:inherit}
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
.w.exact{box-shadow:inset 0 -2px #4da3ff}
.w.manual{outline:1px solid #c792ea}
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
  <h1>smile</h1>
  <span class="tag">فاز ۲ · معلم خودکار</span>
  <span class="tag" title="تاریخ کامپایل — اگر قدیمی است، دوباره بساز">بیلد )HTML" __DATE__ " " __TIME__ R"HTML(</span>
  <span class="tag" id="vt">—</span>
  <span style="flex:1"></span>
  <button id="pause">توقف</button>
  <label class="dim">سرعت <input type="range" id="spd" min="0" max="20" value="10"><b id="spdv">۱×</b></label>
  <label class="dim">دما <input type="range" id="tmp" min="0" max="255" value="100"><b id="tmpv">100</b></label>
  <label class="dim">پرحرفی <input type="range" id="tlk" min="10" max="400" step="10" value="100"><b id="tlkv">۱۰۰٪</b></label>
  <label class="dim">سقف CPU <input type="range" id="cpu" min="10" max="100" step="5" value="100"><b id="cpuv">۱۰۰٪</b></label>
  <label class="dim">نخ <input type="number" id="thr" min="0" max="128" value="0" style="width:52px"><b id="thrv"></b></label>
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
    <div class="card"><div class="lbl">پردازنده</div><div class="val" id="cpuu">—</div><div class="sub" id="thrs"></div></div>
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
      <span>کلمات <b id="wt">۰</b></span>
      <span>داوری خودکار <b id="wat">۰</b></span>
      <span>دستی <b id="wmt">۰</b></span>
      <span>عضو واژه‌نامه <b id="wex">۰</b> (<b id="wexr">۰٪</b>)</span>
      <span>میانگین کیفیت <b id="wq">۰</b>/۱۰۰</span>
      <span>میانگین سیگنال <b id="waa">۰</b></span>
      <span>خط پایه <b id="wbase">۰</b></span>
      <span>خودکار +/−/۰ <b id="wpn">۰/۰/۰</b></span>
      <span>پلاستیسیته <b id="pavg">۰</b> · نورون +/− <b id="ppn">۰/۰</b></span>
      <label class="auto"><input type="checkbox" id="autoscroll" checked> دنبال کردن</label>
    </div>
    <div class="teacherbar">
      <label>معلم
        <select id="teacher">
          <option value="0">خاموش</option><option value="1">املایی</option>
          <option value="2">دیکشنری</option><option value="3" selected>ترکیبی</option>
        </select>
      </label>
      <label>قدرت آزمایشی <input type="range" id="tstr" min="0" max="100" value="0"><b id="tstrv">۰٪</b></label>
      <span class="data" id="tdata">در حال بارگذاری فایل داده…</span>
    </div>
    <div class="teacher-types">
      <div class="teacher-type"><b>۱ · املایی</b><span>طبیعی‌بودن ترتیب حروف را با مدل دوحرفی/سه‌حرفی می‌سنجد؛ لازم نیست واژه دقیقاً در دیکشنری باشد.</span><small id="tstat1">هنوز استفاده نشده</small></div>
      <div class="teacher-type"><b>۲ · دیکشنری</b><span>عضویت دقیق و فراوانی واژه را می‌سنجد؛ واژه‌ی ناشناخته امتیاز دیکشنری صفر می‌گیرد.</span><small id="tstat2">هنوز استفاده نشده</small></div>
      <div class="teacher-type"><b>۳ · ترکیبی</b><span>۵۵٪ امتیاز املایی + ۴۵٪ امتیاز دیکشنری؛ حالت پیش‌فرض برای مشاهده و ارزیابی.</span><small id="tstat3">هنوز استفاده نشده</small></div>
    </div>

    <div id="stream" class="stream"></div>

    <div class="judge-note">ریز داوری — در قدرت ۰ فقط نمره‌ها اندازه‌گیری می‌شوند. قدرت بالاتر، plasticity آزمایشی را فعال می‌کند؛ آزمون A/B فعلی هنوز بهبود قابل اتکا نشان نداده است.</div>
    <div class="judge-wrap">
      <table class="judge-table"><thead><tr>
        <th>زمان</th><th>واژه</th><th>نوع معلم</th><th>املا</th><th>دیکشنری</th>
        <th>کیفیت نهایی</th><th>خط پایه</th><th>مزیت</th><th>سیگنال معلم</th>
        <th>نمره دستی</th><th>عضو؟</th><th>ردپا</th>
      </tr></thead><tbody id="judgeRows"><tr><td colspan="12" class="zero">هنوز واژه‌ای داوری نشده…</td></tr></tbody></table>
    </div>

    <div class="composer">
      <input id="msg" type="text" placeholder="چیزی به مدل بگو…" autocomplete="off">
      <button class="p" id="send">بگو</button>
    </div>
    <div class="hint">
      روی هر کلمه کلیک کن تا نمره بدهی · تنبیه سریع با کلیک راست: <b>−۱۰</b>
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
const TMODE=['خاموش','املایی','دیکشنری','ترکیبی'];
const fa=n=>(Number(n)||0).toLocaleString('fa-IR');
const fnum=(n,d=2)=>(Number(n)||0).toLocaleString('fa-IR',{minimumFractionDigits:d,maximumFractionDigits:d});
const signed=(n,d=3)=>{n=Number(n)||0;return (n>0?'+':'')+fnum(n,d);};
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
function renderTeacherStats(s){
  for(let m=1;m<=3;m++){
    const c=(s.tcount&&s.tcount[m])||0;
    document.getElementById('tstat'+m).textContent=c
      ? fa(c)+' واژه · میانگین کیفیت '+fnum((s.tqavg&&s.tqavg[m])||0,1)+
        ' · میانگین سیگنال '+signed((s.travg&&s.travg[m])||0,3)+
        ' · خط پایه '+fnum((s.tbases&&s.tbases[m])||0,1)
      : 'هنوز استفاده نشده';
  }
}
function renderJudgeTable(words){
  const body=document.getElementById('judgeRows');
  const rows=(words||[]).filter(w=>w.a||w.m).slice(-25).reverse();
  if(!rows.length){body.innerHTML='<tr><td colspan="12" class="zero">هنوز واژه‌ای داوری نشده…</td></tr>';return;}
  body.innerHTML=rows.map(w=>{
    const auto=!!w.a, manual=!!w.m, ar=Number(w.ar)||0, mr=Number(w.mr)||0;
    const rc=ar>0?'pos':(ar<0?'neg':'zero');
    const mc=mr>0?'pos manualscore':(mr<0?'neg manualscore':'manualscore');
    return `<tr>
      <td>${fnum(w.t,1)}s</td><td class="wordcell">${esc(w.w)}</td>
      <td>${auto?TMODE[w.tm||0]:'—'}</td>
      <td>${auto?fa(w.oq||0):'—'}</td><td>${auto?fa(w.dq||0):'—'}</td>
      <td><b>${auto?fa(w.q||0):'—'}</b></td><td>${auto?fnum(w.bl||0,1):'—'}</td>
      <td>${auto?signed(w.av||0,1):'—'}</td><td class="${rc}">${auto?signed(ar,3):'—'}</td>
      <td class="${mc}">${manual?signed(mr,1):'—'}</td>
      <td class="${w.x?'exactyes':'zero'}">${w.x?'بله':'خیر'}</td><td>${fa(w.tr||0)}</td>
    </tr>`;
  }).join('');
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
  document.getElementById('cpuu').textContent=fa(Math.round(s.cpuused||0))+'٪';
  document.getElementById('thrs').textContent=fa(s.threads||1)+' نخ · سقف '+fa(s.cpulimit||100)+'٪ · سرعت '+(s.vspeed||0).toFixed(2)+'×';
  document.getElementById('thrv').textContent=(document.getElementById('thr').value=='0'?'(خودکار: '+fa(s.threads)+')':'');
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
  renderJudgeTable(s.words||[]);
  renderTeacherStats(s);
  document.getElementById('wt').textContent=fa(s.wtotal||0);
  document.getElementById('wat').textContent=fa(s.wauto||0);
  document.getElementById('wmt').textContent=fa(s.wmanual||0);
  document.getElementById('wex').textContent=fa(s.wexact||0);
  document.getElementById('wexr').textContent=fnum((s.wauto||0)?(s.wexact||0)*100/s.wauto:0,1)+'٪';
  document.getElementById('wq').textContent=fnum(s.wquality||0,1);
  document.getElementById('waa').textContent=signed(s.aavg||0,3);
  document.getElementById('wbase').textContent=fnum(s.tbase||0,1);
  document.getElementById('wpn').textContent=fa(s.wpos||0)+' / '+fa(s.wneg||0)+' / '+fa(s.wzero||0);
  document.getElementById('pavg').textContent=signed(s.pavg||0,1);
  document.getElementById('ppn').textContent=fa(s.ppos||0)+' / '+fa(s.pneg||0);
  document.getElementById('teacher').value=String(s.teacher||0);
  document.getElementById('tstr').value=String(s.tstrength||0);
  document.getElementById('tstrv').textContent=fa(s.tstrength||0)+'٪';
  const td=document.getElementById('tdata');
  td.textContent=s.tloaded
    ? fa(s.lexwords||0)+' تایید · '+fa(s.lexsuggest||0)+' پیشنهاد · '+fa(s.lexblocked||0)+' مسدود · پاداش خالص '+signed(s.areward||0,2)
    : 'فایل persian_words.tsv پیدا نشد — معلم خاموش است';
  td.className='data '+(s.tloaded?'ok':'bad');
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
  (s.words||[]).forEach(w=>items.push({t:w.t,human:0,text:w.w,id:w.id,sc:w.s,done:w.d,
      q:w.q||0,oq:w.oq||0,dq:w.dq||0,tm:w.tm||0,bl:w.bl||0,av:w.av||0,
      ar:w.ar||0,mr:w.mr||0,x:w.x,a:w.a,m:w.m,tr:w.tr||0}));
  items.sort((a,b)=>a.t-b.t);
  const key=items.map(i=>i.id+':'+(i.sc||0)+(i.q||0)+(i.ar||0)+(i.mr||0)).join(',');
  if(key===lastKey)return;
  lastKey=key;
  const near=el.scrollHeight-el.scrollTop-el.clientHeight<80;
  let h='';
  for(const it of items){
    if(it.human){ h+=`<div class="hw">${esc(it.text)}</div>`; continue; }
    let cls=it.done?(it.sc>0?'w sc':(it.sc<0?'w sn':'w')):'w';
    if(it.x)cls+=' exact'; if(it.m)cls+=' manual';
    const teacherBadge=it.a?`<span class="b">معلم ${signed(it.ar,2)}</span><span class="b">Q${fa(it.q)}</span>`:'';
    const manualBadge=it.m?`<span class="b">دستی ${signed(it.mr,1)}</span>`:'';
    const title=it.a
      ? `${TMODE[it.tm]} · املا ${it.oq} · دیکشنری ${it.dq} · نهایی ${it.q} · خط پایه ${fnum(it.bl,1)} · مزیت ${signed(it.av,1)} · سیگنال ${signed(it.ar,3)} · ردپا ${it.tr}`
      : `بدون داوری خودکار · ردپا ${it.tr}`;
    h+=`<span class="${cls}" title="${title}" data-id="${it.id}" data-w="${esc(it.text)}">${esc(it.text)}${teacherBadge}${manualBadge}</span>`;
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
  e.preventDefault(); score(w.dataset.id,-10);
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
document.getElementById('cpu').oninput=e=>{
  document.getElementById('cpuv').textContent=fa(+e.target.value)+'٪';
  fetch('/cpu?v='+e.target.value);};
document.getElementById('thr').onchange=e=>fetch('/threads?v='+e.target.value);
document.getElementById('tlk').oninput=e=>{
  document.getElementById('tlkv').textContent=fa(+e.target.value)+'٪';
  fetch('/talk?v='+e.target.value);};
document.getElementById('teacher').onchange=e=>fetch('/teacher?v='+e.target.value);
document.getElementById('tstr').oninput=e=>{
  document.getElementById('tstrv').textContent=fa(+e.target.value)+'٪';
  fetch('/teacher-strength?v='+e.target.value);};
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
        "\"mana\":%.1f,\"transit\":%.1f,\"events\":%lld,\"evrate\":%.1f,\"faults\":%lld,"
        "\"threads\":%d,\"cpulimit\":%d,\"cpuused\":%.1f,\"vspeed\":%.3f,",
        (double)s.vtime_us / SEC, (long long)s.alive, (long long)s.dead,
        (long long)s.healthy, (long long)s.ignoring, (long long)s.spamming,
        (long long)s.dormant, (long long)s.asleep, s.fire_hz,
        (double)s.total_mana / MANA, (double)s.transit / MANA,
        (long long)s.events, s.wall_s > 0 ? s.events / s.wall_s : 0.0,
        (long long)s.faults,
        effective_threads(), g_cpu_percent.load(), g_cpu_measured.load(),
        g_virtual_speed.load());
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

    snprintf(buf, sizeof buf,
             "\"wtotal\":%lld,\"wscored\":%lld,\"wavg\":%.3f,"
             "\"wauto\":%lld,\"wmanual\":%lld,\"wexact\":%lld,\"wquality\":%.2f,"
             "\"wpos\":%lld,\"wneg\":%lld,\"wzero\":%lld,"
             "\"pavg\":%.2f,\"ppos\":%lld,\"pneg\":%lld,"
             "\"areward\":%.3f,\"aavg\":%.4f,\"tbase\":%.2f,"
             "\"teacher\":%d,\"tstrength\":%d,\"tloaded\":%s,"
             "\"lexwords\":%zu,\"lexsuggest\":%zu,\"lexblocked\":%zu,",
             (long long)s.words_total, (long long)s.words_scored, s.avg_score,
             (long long)s.words_auto, (long long)s.words_manual, (long long)s.words_exact,
             s.avg_quality, (long long)s.words_positive, (long long)s.words_negative,
             (long long)s.words_neutral, s.plasticity_avg,
             (long long)s.plasticity_positive, (long long)s.plasticity_negative,
             (double)s.auto_reward_total / MANA,
             s.words_auto ? (double)s.auto_reward_total / s.words_auto / MANA : 0.0,
             s.teacher_baseline, g_teacher_mode.load(), g_teacher_strength.load(),
             g_lexicon.loaded ? "true" : "false", g_lexicon.verified.size(),
             g_lexicon.suggested.size(), g_lexicon.blocked.size());
    j += buf;

    j += "\"tcount\":[";
    for (int m = 0; m < 4; ++m) { snprintf(buf,sizeof buf,"%s%lld",m?",":"",(long long)s.teacher_count[m]); j+=buf; }
    j += "],\"tbases\":[";
    for (int m = 0; m < 4; ++m) { snprintf(buf,sizeof buf,"%s%.3f",m?",":"",s.teacher_baseline_by_mode[m]); j+=buf; }
    j += "],\"tqavg\":[";
    for (int m = 0; m < 4; ++m) { snprintf(buf,sizeof buf,"%s%.3f",m?",":"",s.teacher_quality_avg[m]); j+=buf; }
    j += "],\"travg\":[";
    for (int m = 0; m < 4; ++m) { snprintf(buf,sizeof buf,"%s%.4f",m?",":"",s.teacher_reward_avg[m]); j+=buf; }
    j += "],";

    j += "\"words\":[";
    for (size_t i = 0; i < s.words.size(); ++i) {
        const OutWord& w = s.words[i];
        snprintf(buf, sizeof buf,
                 "%s{\"id\":%u,\"t\":%.1f,\"s\":%.3f,\"d\":%s,"
                 "\"q\":%d,\"oq\":%d,\"dq\":%d,\"tm\":%u,"
                 "\"bl\":%.2f,\"av\":%.2f,\"ar\":%.3f,\"mr\":%.3f,"
                 "\"x\":%s,\"a\":%s,\"m\":%s,\"tr\":%zu,\"w\":\"",
                 i ? "," : "", w.id, w.t, (double)w.score_milli / MANA,
                 w.scored ? "true" : "false", w.quality,
                 w.spelling_quality, w.dictionary_quality, (unsigned)w.teacher_mode,
                 w.teacher_baseline, w.teacher_advantage,
                 (double)w.auto_reward / MANA, (double)w.manual_reward / MANA,
                 w.exact ? "true" : "false", w.auto_scored ? "true" : "false",
                 w.manual_scored ? "true" : "false", w.trace.size());
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
        } else if (R.rfind("GET /threads", 0) == 0) {
            g_threads.store(std::max(0, std::min(256, qparam(R, "v", 0))));
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /cpu", 0) == 0) {
            g_cpu_percent.store(std::max(10, std::min(100, qparam(R, "v", 100))));
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /talk", 0) == 0) {
            g_talkativeness.store(std::max(10, std::min(400, qparam(R, "v", 100))));
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /teacher-strength", 0) == 0) {
            g_teacher_strength.store(std::max(0, std::min(100, qparam(R, "v", 0))));
            body = "{\"ok\":1}";
        } else if (R.rfind("GET /teacher", 0) == 0) {
            int mode = std::max(0, std::min(3, qparam(R, "v", 0)));
            g_teacher_mode.store(g_lexicon.loaded ? mode : 0);
            body = "{\"ok\":1}";
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
    int  N = 32000, port = 8420, headless_s = 0;
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
        else if (a == "--threads")    g_threads.store(atoi(nxt()));
        else if (a == "--cpu")        g_cpu_percent.store(std::max(10, std::min(100, atoi(nxt()))));
        else if (a == "--words")      g_words_path = nxt();
        else if (a == "--user-words") g_user_words_path = nxt();
        else if (a == "--teacher-strength")
            g_teacher_strength.store(std::max(0, std::min(100, atoi(nxt()))));
        else if (a == "--teacher") {
            std::string v = nxt();
            int m = (v == "off" || v == "خاموش") ? 0 :
                    (v == "spelling") ? 1 : (v == "dictionary") ? 2 :
                    (v == "combined") ? 3 : atoi(v.c_str());
            g_teacher_mode.store(std::max(0, std::min(3, m)));
        }
    }

    printf("\n");
    printf("  ╔═══════════════════════════════════════════════╗\n");
    printf("  ║   smile — فاز ۲                              ║\n");
    printf("  ║   یک مغز دیجیتال رویدادمحور                   ║\n");
    printf("  ╚═══════════════════════════════════════════════╝\n\n");

    // فایل داده کنار exe یا در مسیر کاری. نبودنش مغز را نمی‌کشد؛ فقط معلم
    // خودکار خاموش می‌شود و نمره‌دهی دستی همچنان کار می‌کند.
    bool teacher_ok = load_teacher_data(g_words_path);
    if (!teacher_ok && g_words_path == "persian_words.tsv") {
        std::string exe = argc > 0 ? argv[0] : "";
        size_t slash = exe.find_last_of("/\\");
        if (slash != std::string::npos)
            teacher_ok = load_teacher_data(exe.substr(0, slash + 1) + g_words_path);
    }
    if (teacher_ok) {
        printf("  معلم فارسی: %zu تاییدشده · %zu پیشنهادی · %zu مسدود  (حالت %d · قدرت %d%%)\n",
               g_lexicon.verified.size(), g_lexicon.suggested.size(), g_lexicon.blocked.size(),
               g_teacher_mode.load(), g_teacher_strength.load());
        printf("  داده: %s\n", g_lexicon.loaded_path.c_str());
    } else {
        printf("  [!] %s — معلم خودکار خاموش شد.\n", g_lexicon.error.c_str());
        g_teacher_mode.store(0);
    }

    bool checkpoint_loaded = loadf && load_brain(loadf);
    if (checkpoint_loaded && B.n.size() == (size_t)N) {
        printf("  بارگذاری از چک‌پوینت: %s  (%zu نورون)\n", loadf, B.n.size());
    } else {
        if (checkpoint_loaded) {
            printf("  [!] چک‌پوینت %zu نورون دارد؛ اندازه‌ی درخواستی %d است. مغز تازه ساخته می‌شود.\n",
                   B.n.size(), N);
        }
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
        if (B.words_auto > 0) {
            size_t k = std::min<size_t>(10, B.words.size());
            double q_first = 0, q_last = 0;
            for (size_t i = 0; i < k; ++i) {
                q_first += B.words[i].quality;
                q_last += B.words[B.words.size() - k + i].quality;
            }
            printf("  گزارش معلم: words=%lld exact=%lld (%.2f%%) avgQ=%.2f "
                   "first%zuQ=%.2f last%zuQ=%.2f avgSignal=%+.4f\n",
                   (long long)B.words_auto, (long long)B.words_exact,
                   100.0 * B.words_exact / std::max<i64>(1, B.words_auto),
                   B.quality_sum / std::max<i64>(1, B.words_auto),
                   k, k ? q_first / k : 0.0, k, k ? q_last / k : 0.0,
                   (double)B.auto_reward_total / std::max<i64>(1, B.words_auto) / MANA);
            i64 psum = 0, ppos = 0, pneg = 0;
            for (const auto& nu : B.n) {
                psum += nu.plasticity;
                if (nu.plasticity > 0) ppos++; else if (nu.plasticity < 0) pneg++;
            }
            printf("  پلاستیسیته: avg=%+.2f positive=%lld negative=%lld\n",
                   (double)psum / std::max<size_t>(1, B.n.size()),
                   (long long)ppos, (long long)pneg);
        }
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
