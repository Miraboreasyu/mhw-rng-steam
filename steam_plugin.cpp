// ============================================================================
//  MHW RNG Official Plugin: Steamworks Key Hint & Sequence Controller (`rng_steam.dll`)
//  Version : 1.0.0 (SDK v3.0 Showcase Implementation)
//  Targets (Verified on MHW:I 15.23 Steam build 15539686):
//    1. `RVA 0x066679A` (`cSteamControl::start` `CALL MtRandom::nrand` #1)
//       Generates `pos1 = r1 % 3` (`order[pos1] = 1`), reads `cSteamControl*` from `callerRegs->rbx`.
//    2. `RVA 0x06667BB` (`cSteamControl::start` `CALL MtRandom::nrand` #2)
//       Generates `pos2 = (pos1 + (r2 & 1) + 1) % 3` (`order[pos2] = 2`), remaining slot = `0`.
//       Verified button IDs: `0 = Left (A/X/Square)`, `1 = Middle (W/Y/Triangle)`, `2 = Right (D/B/Circle)`.
//    3. `RVA 0x05CA0A2` (`FUN_1405ca060` called by `cSteamControl::start` @ `0x140666811`)
//       Hidden guaranteed-win pity roll: `(r % 100) < *(uint32_t*)callerRegs->rbx` (`ctrl + 0x4A4`).
//
//  SDK v3.0 Capabilities Showcased:
//    - Dual-mode single-plugin architecture: works out of the box in BOTH `ObserveOnly` (`$rng observe`)
//      for pure non-invasive key hints and `Takeover` (`$rng on`) for inverse-solved sequence locking.
//    - MASM non-volatile register capture (`ctx->callerRegs->rbx / rdi`) + SEH-guarded game struct reads.
//    - Synchronous `preFn` / `postFn` (`kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly`).
//    - Main-thread safe async chat delivery (`ChatPrintfAsync`) drained smoothly by `sChat::move`.
//    - Kernel lifecycle subscription (`RegisterLifecycleHook`) for `$rng clearall` / `$rng reset` sync.
// ============================================================================
#include "mhw_rng_api.h"

#include <windows.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

namespace {

// Verified MHW:I 15.23 (Steam 15539686) RVAs & `cSteamControl` offsets:
constexpr std::uint32_t  kRvaSteamCode1        = 0x066679AU; // cSteamControl::start -> CALL MtRandom::nrand #1
constexpr std::uint32_t  kRvaSteamCode2        = 0x06667BBU; // cSteamControl::start -> CALL MtRandom::nrand #2
constexpr std::uint32_t  kRvaSteamAutoWin      = 0x05CA0A2U; // FUN_1405ca060 -> CALL MtRandom::nrand

constexpr std::uintptr_t kOffCtrlOutputMode    = 0x0440U;    // uint32_t (0 = 1x, 1 = 10x)
constexpr std::uintptr_t kOffCtrlStreak        = 0x0480U;    // uint8_t
constexpr std::uintptr_t kOffCtrlPityPct       = 0x04A4U;    // uint32_t (12..100)
constexpr std::uintptr_t kOffCtrlIsRedRound    = 0x04A9U;    // uint8_t  (1 = red high-output bonus round)

enum class HintStyle : std::uint8_t {
    kKeyboard = 0, // A - W - D (左-中-右)
    kChinese  = 1, // 左 - 中 - 右
    kXbox     = 2, // X - Y - B (左-中-右)
    kPlaySta  = 3, // 方 - 三 - 圆 (左-中-右)
    kNumeric  = 4  // 1 - 2 - 3
};

struct PermDef {
    std::uint8_t b[3];
    const char*  awdCode; // e.g. "awd"
};

// All 6 permutations of {0, 1, 2} in canonical order:
constexpr PermDef kPerms[6] = {
    { {0U, 1U, 2U}, "awd" },
    { {0U, 2U, 1U}, "adw" },
    { {1U, 0U, 2U}, "wad" },
    { {1U, 2U, 0U}, "wda" },
    { {2U, 0U, 1U}, "daw" },
    { {2U, 1U, 0U}, "dwa" }
};

struct SteamCtrlTelemetry {
    bool          validCtrl  = false;
    bool          isRedRound = false;
    bool          is10xMode  = false;
    std::uint8_t  streak     = 0U;
    std::uint32_t pityPct    = 0U;
};

struct TlsCode1State {
    bool           valid   = false;
    std::uintptr_t ctrlPtr = 0ULL;
    std::uint32_t  rawU32  = 0U;
    std::uint32_t  pos1    = 0U;
};

struct TlsRoundCorrelator {
    bool           valid   = false;
    std::uintptr_t ctrlPtr = 0ULL;
    std::uint64_t  roundNo = 0ULL;
};

const MhwRngHostApi* g_host   = nullptr;
MhwRngPluginId       g_selfId = 0U;
MhwRngHookHandle     g_hookCode1   = kMhwRngInvalidHandle;
MhwRngHookHandle     g_hookCode2   = kMhwRngInvalidHandle;
MhwRngHookHandle     g_hookAutoWin = kMhwRngInvalidHandle;
MhwRngHookHandle     g_hookLife    = kMhwRngInvalidHandle;

// Strict leaf mutex: NEVER call `g_host->...` while holding `g_stateMx`!
std::mutex           g_stateMx;
bool                 g_hintEnabled   = true;
bool                 g_notifyAutoWin = true;
bool                 g_verbose       = false;
HintStyle            g_style         = HintStyle::kKeyboard;
bool                 g_lockEnabled   = false;
std::array<std::uint8_t, 3> g_lockOrder{0U, 1U, 2U};
std::uint32_t        g_lockCondId    = 1U; // (b0*9 + b1*3 + b2) + 1

std::uint64_t        g_roundCount    = 0ULL;
std::uint64_t        g_redCount      = 0ULL;
std::uint64_t        g_autoWinCount  = 0ULL;
std::uint64_t        g_permCounts[6]{};

bool                 g_lastValid      = false;
std::array<std::uint8_t, 3> g_lastOrder{0U, 1U, 2U};
bool                 g_lastIsRed      = false;
bool                 g_lastIs10x      = false;
bool                 g_lastOverridden = false;
bool                 g_lastAutoWin    = false;
std::uint8_t         g_lastStreak     = 0U;
std::uint32_t        g_lastPityPct    = 0U;

thread_local TlsCode1State      tls_code1{};
thread_local TlsRoundCorrelator tls_lastRound{};

std::string LowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    return s;
}

bool IsValidUserPtr(std::uintptr_t p, std::uintptr_t alignMask = 0x7ULL) noexcept {
    return p >= 0x100000ULL && p <= 0x00007FFFFFFF0000ULL && (p & alignMask) == 0ULL;
}

int FindPermIndex(const std::array<std::uint8_t, 3>& ord) noexcept {
    for (int i = 0; i < 6; ++i) {
        if (kPerms[i].b[0] == ord[0] && kPerms[i].b[1] == ord[1] && kPerms[i].b[2] == ord[2]) {
            return i;
        }
    }
    return 0;
}

std::uint32_t MakeOrderCondId(const std::array<std::uint8_t, 3>& ord) noexcept {
    return static_cast<std::uint32_t>(ord[0] * 9U + ord[1] * 3U + ord[2] + 1U);
}

const char* ButtonLabel(std::uint8_t btnId, HintStyle style) noexcept {
    const std::size_t idx = (btnId < 3U) ? static_cast<std::size_t>(btnId) : 0U;
    static constexpr const char* kKb[3]   = { "A", "W", "D" };
    static constexpr const char* kCn[3]   = { u8"左", u8"中", u8"右" };
    static constexpr const char* kXbox[3] = { "X", "Y", "B" };
    static constexpr const char* kPs[3]   = { u8"方", u8"三", u8"圆" };
    static constexpr const char* kNum[3]  = { "1", "2", "3" };
    switch (style) {
        case HintStyle::kChinese:  return kCn[idx];
        case HintStyle::kXbox:     return kXbox[idx];
        case HintStyle::kPlaySta:  return kPs[idx];
        case HintStyle::kNumeric:  return kNum[idx];
        default:                   return kKb[idx];
    }
}

const char* StyleName(HintStyle style, bool en) noexcept {
    switch (style) {
        case HintStyle::kChinese:  return en ? "cn(L-M-R)"      : u8"cn(左-中-右)";
        case HintStyle::kXbox:     return "xbox(X-Y-B)";
        case HintStyle::kPlaySta:  return en ? "ps(Sq-Tri-Cir)" : u8"ps(方-三-圆)";
        case HintStyle::kNumeric:  return "num(1-2-3)";
        default:                   return "kb(A-W-D)";
    }
}

std::string FormatOrderText(const std::array<std::uint8_t, 3>& ord, HintStyle style, bool en) {
    char buf[96]{};
    const char* s0 = ButtonLabel(ord[0], style);
    const char* s1 = ButtonLabel(ord[1], style);
    const char* s2 = ButtonLabel(ord[2], style);
    if (style == HintStyle::kChinese || style == HintStyle::kNumeric) {
        std::snprintf(buf, sizeof(buf), "%s - %s - %s", s0, s1, s2);
    } else if (en) {
        static constexpr const char* kLmr[3] = { "L", "M", "R" };
        std::snprintf(buf, sizeof(buf), "%s - %s - %s (%s-%s-%s)",
                      s0, s1, s2, kLmr[ord[0]], kLmr[ord[1]], kLmr[ord[2]]);
    } else {
        std::snprintf(buf, sizeof(buf), "%s - %s - %s (%s-%s-%s)",
                      s0, s1, s2,
                      ButtonLabel(ord[0], HintStyle::kChinese),
                      ButtonLabel(ord[1], HintStyle::kChinese),
                      ButtonLabel(ord[2], HintStyle::kChinese));
    }
    return buf;
}

bool ParsePermOrder(const std::string& rawTok, std::array<std::uint8_t, 3>& outOrd) {
    const std::string low = LowerAscii(rawTok);
    if (low == "r2" || low == "space" || low == "auto") {
        outOrd = {0U, 1U, 2U};
        return true;
    }

    // Compact 3-char tokens ("awd", "123", "012", "lcr", "xyb") or separated tokens ("a-w-d", "1,2,3")
    std::uint8_t parsed[3]{};
    std::size_t  n = 0U;
    for (char ch : low) {
        if (ch == '-' || ch == ',' || ch == '/' || ch == ' ') continue;
        if (n >= 3U) return false;
        if (ch == 'a' || ch == 'l' || ch == 'x' || ch == '0' || ch == '1') {
            if (ch == '1' && (low.find('0') != std::string::npos)) {
                parsed[n++] = 1U;
            } else if (ch == '1') {
                // Disambiguate "012" vs "123" once we inspect all digits; handle cleanly below
                parsed[n++] = 0xFFU;
            } else {
                parsed[n++] = 0U;
            }
        } else if (ch == 'w' || ch == 'c' || ch == 'm' || ch == 'y' || ch == '2') {
            parsed[n++] = (ch == '2') ? 0xFEU : 1U;
        } else if (ch == 'd' || ch == 'r' || ch == 'b' || ch == '3') {
            parsed[n++] = (ch == '3') ? 0xFDU : 2U;
        } else {
            return false;
        }
    }
    if (n != 3U) return false;

    // Resolve '0'/'1'/'2' vs '1'/'2'/'3'
    const bool hasZero  = (low.find('0') != std::string::npos);
    const bool hasThree = (low.find('3') != std::string::npos);
    if (hasZero && hasThree) return false;
    for (std::uint8_t& v : parsed) {
        if (v == 0xFFU) v = hasZero ? 1U : 0U;
        else if (v == 0xFEU) v = hasZero ? 2U : 1U;
        else if (v == 0xFDU) v = 2U;
    }

    bool seen[3]{};
    for (std::uint8_t v : parsed) {
        if (v >= 3U || seen[v]) return false;
        seen[v] = true;
    }
    outOrd = {parsed[0], parsed[1], parsed[2]};
    return true;
}

void ReadSteamCtrlTelemetrySEH(std::uintptr_t ctrlPtr, SteamCtrlTelemetry& out) noexcept {
    if (!IsValidUserPtr(ctrlPtr)) return;
    __try {
        const auto mode   = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffCtrlOutputMode);
        const auto streak = *reinterpret_cast<const std::uint8_t*>(ctrlPtr + kOffCtrlStreak);
        const auto pity   = *reinterpret_cast<const std::uint32_t*>(ctrlPtr + kOffCtrlPityPct);
        const auto isRed  = *reinterpret_cast<const std::uint8_t*>(ctrlPtr + kOffCtrlIsRedRound);
        if (mode <= 1U && pity <= 100U && isRed <= 1U) {
            out.validCtrl  = true;
            out.is10xMode  = (mode == 1U);
            out.streak     = streak;
            out.pityPct    = pity;
            out.isRedRound = (isRed != 0U);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ReadAutoWinPityPctSEH(std::uintptr_t pityPtr, std::uint32_t& outPct) noexcept {
    if (!IsValidUserPtr(pityPtr, 0x3ULL)) return false;
    __try {
        const std::uint32_t pct = *reinterpret_cast<const std::uint32_t*>(pityPtr);
        if (pct <= 100U) {
            outPct = pct;
            return true;
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Finds `idx` where `ord[idx] == val` (val in {0, 1, 2}).
std::uint32_t FindOrderValuePos(const std::array<std::uint8_t, 3>& ord, std::uint8_t val) noexcept {
    for (std::uint32_t i = 0U; i < 3U; ++i) {
        if (ord[i] == val) return i;
    }
    return 0U;
}

// ============================================================================
//  Synchronous Pull Hooks (`0x066679A`, `0x06667BB`, `0x05CA0A2`)
// ============================================================================

void __fastcall OnSteamCode1Pre(const MhwRngDrawContext*,
                                MhwRngHookDecision* ioDecision,
                                void*) {
    if (!ioDecision) return;
    bool locked = false;
    std::array<std::uint8_t, 3> want{};
    std::uint32_t condId = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        locked = g_lockEnabled;
        want   = g_lockOrder;
        condId = g_lockCondId;
    }
    if (!locked) {
        ioDecision->action = kMhwRngActionContinue;
        return;
    }
    ioDecision->action = kMhwRngActionReturnValue;
    ioDecision->condId = condId;
    ioDecision->value0 = FindOrderValuePos(want, 1U); // pos1 = r1 % 3
}

void __fastcall OnSteamCode1Post(const MhwRngDrawContext* ctx,
                                 MhwRngHookDecision*,
                                 void*) {
    if (!ctx) return;
    tls_code1.valid   = true;
    tls_code1.ctrlPtr = (ctx->callerRegs && (ctx->callFlags & kMhwRngDrawFlagHasRegs) != 0U)
                            ? static_cast<std::uintptr_t>(ctx->callerRegs->rbx)
                            : 0ULL;
    tls_code1.rawU32  = ctx->finalValue0;
    tls_code1.pos1    = ctx->finalValue0 % 3U;
}

void __fastcall OnSteamCode2Pre(const MhwRngDrawContext*,
                                MhwRngHookDecision* ioDecision,
                                void*) {
    if (!ioDecision) return;
    bool locked = false;
    std::array<std::uint8_t, 3> want{};
    std::uint32_t condId = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        locked = g_lockEnabled;
        want   = g_lockOrder;
        condId = g_lockCondId;
    }
    if (!locked) {
        ioDecision->action = kMhwRngActionContinue;
        return;
    }
    const std::uint32_t pos1  = FindOrderValuePos(want, 1U);
    const std::uint32_t pos2  = FindOrderValuePos(want, 2U);
    const std::uint32_t delta = (pos2 + 3U - pos1) % 3U; // 1 or 2
    ioDecision->action = kMhwRngActionReturnValue;
    ioDecision->condId = condId;
    ioDecision->value0 = (delta >= 1U) ? (delta - 1U) : 0U; // (r2 & 1) + 1 == delta
}

void __fastcall OnSteamCode2Post(const MhwRngDrawContext* ctx,
                                 MhwRngHookDecision*,
                                 void*) {
    if (!ctx) return;

    const bool hasRegs = (ctx->callerRegs && (ctx->callFlags & kMhwRngDrawFlagHasRegs) != 0U);
    const std::uintptr_t ctrlPtr = hasRegs
                                       ? static_cast<std::uintptr_t>(ctx->callerRegs->rbx)
                                       : tls_code1.ctrlPtr;

    std::uint32_t pos1 = 0U;
    if (hasRegs && ctx->callerRegs->rdi <= 2ULL) {
        pos1 = static_cast<std::uint32_t>(ctx->callerRegs->rdi);
    } else if (tls_code1.valid) {
        pos1 = tls_code1.pos1;
    }
    tls_code1.valid = false;

    const std::uint32_t pos2 = (pos1 + (ctx->finalValue0 & 1U) + 1U) % 3U;
    std::array<std::uint8_t, 3> ord{0U, 0U, 0U};
    ord[pos1] = 1U;
    ord[pos2] = 2U;

    SteamCtrlTelemetry tel{};
    ReadSteamCtrlTelemetrySEH(ctrlPtr, tel);

    const bool isOverridden = ((ctx->callFlags & kMhwRngDrawFlagOverridden) != 0U);
    const int  permIdx      = FindPermIndex(ord);

    bool          hintEnabled = false;
    bool          lockEnabled = false;
    bool          verbose     = false;
    HintStyle     style       = HintStyle::kKeyboard;
    std::uint64_t roundNo     = 0ULL;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        ++g_roundCount;
        roundNo = g_roundCount;
        if (tel.validCtrl && tel.isRedRound) ++g_redCount;
        ++g_permCounts[permIdx];

        g_lastValid      = true;
        g_lastOrder      = ord;
        g_lastIsRed      = tel.validCtrl && tel.isRedRound;
        g_lastIs10x      = tel.validCtrl && tel.is10xMode;
        g_lastOverridden = isOverridden;
        g_lastAutoWin    = false;
        g_lastStreak     = tel.validCtrl ? tel.streak : 0U;
        if (tel.validCtrl) g_lastPityPct = tel.pityPct;

        hintEnabled = g_hintEnabled;
        lockEnabled = g_lockEnabled && isOverridden;
        verbose     = g_verbose;
        style       = g_style;
    }

    tls_lastRound.valid   = true;
    tls_lastRound.ctrlPtr = ctrlPtr;
    tls_lastRound.roundNo = roundNo;

    // Quiet by default when `lockEnabled` (`verbose == false`): unlocked runs still show
    // the 3-key hint so manual Steamworks input works, while locked auto-hold stays quiet.
    if (hintEnabled && g_host && (!lockEnabled || verbose)) {
        const bool en = MhwRng_HostIsEnglish(g_host);
        const std::string keyText = FormatOrderText(ord, style, en);
        const char* redTag  = (tel.validCtrl && tel.isRedRound) ? (en ? " [RED!]" : u8" [红槽!]") : "";
        const char* lockTag = lockEnabled ? (en ? " [LOCK]" : u8" [锁序]") : "";
        g_host->ChatPrintfAsync(en ? "[Steam #%llu] %s%s%s" : u8"[蒸汽机 #%llu] %s%s%s",
                                static_cast<unsigned long long>(roundNo),
                                keyText.c_str(),
                                redTag,
                                lockTag);
    }
}

void __fastcall OnSteamAutoWinPost(const MhwRngDrawContext* ctx,
                                   MhwRngHookDecision*,
                                   void*) {
    if (!ctx) return;
    const bool hasRegs = (ctx->callerRegs && (ctx->callFlags & kMhwRngDrawFlagHasRegs) != 0U);
    const std::uintptr_t pityPtr = hasRegs ? static_cast<std::uintptr_t>(ctx->callerRegs->rbx) : 0ULL;

    std::uint32_t pityPct = 0U;
    if (!ReadAutoWinPityPctSEH(pityPtr, pityPct)) return;

    const std::uint32_t roll = ctx->finalValue0 % 100U;
    const bool triggered = (roll < pityPct);

    bool          hintEnabled   = false;
    bool          notifyAutoWin = false;
    bool          verbose       = false;
    bool          hasRoundNo    = false;
    std::uint64_t roundNo       = 0ULL;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_lastPityPct = pityPct;
        if (triggered) {
            ++g_autoWinCount;
            g_lastAutoWin = true;
        }
        hintEnabled   = g_hintEnabled;
        notifyAutoWin = g_notifyAutoWin;
        verbose       = g_verbose;
        if (tls_lastRound.valid &&
            (tls_lastRound.ctrlPtr == 0ULL || (tls_lastRound.ctrlPtr + kOffCtrlPityPct) == pityPtr)) {
            hasRoundNo = true;
            roundNo    = tls_lastRound.roundNo;
        }
    }

    if (triggered && hintEnabled && notifyAutoWin && verbose && g_host) {
        const bool en = MhwRng_HostIsEnglish(g_host);
        if (hasRoundNo) {
            g_host->ChatPrintfAsync(en ? "[Steam #%llu] Hidden pity AUTO-WIN triggered (any key wins! Pity:%u%%)"
                                       : u8"[蒸汽机 #%llu] 触发隐藏必中保底 (任意键全中! 保底率:%u%%)",
                                    static_cast<unsigned long long>(roundNo),
                                    pityPct);
        } else {
            g_host->ChatPrintfAsync(en ? "[Steam] Hidden pity AUTO-WIN triggered (any key wins! Pity:%u%%)"
                                       : u8"[蒸汽机] 触发隐藏必中保底 (任意键全中! 保底率:%u%%)",
                                    pityPct);
        }
    }
}

// ============================================================================
//  Kernel Lifecycle Hook & Site Status Formatter
// ============================================================================

void __fastcall OnKernelLifecycle(const MhwRngEventInfo* info, void*) {
    if (!info) return;
    if (info->eventKind == kMhwRngEventClearAllRules) {
        std::lock_guard<std::mutex> lk(g_stateMx);
        g_lockEnabled = false;
    }
}

bool __fastcall FormatSteamSlotStatus(const MhwRngSlotSnapshot* snap,
                                      char* outBuf,
                                      std::size_t bufCap,
                                      void*) {
    if (!snap || !outBuf || bufCap == 0U) return false;
    const bool en = MhwRng_HostIsEnglish(g_host);
    std::lock_guard<std::mutex> lk(g_stateMx);
    if (snap->callSiteRva == kRvaSteamAutoWin) {
        std::snprintf(outBuf, bufCap,
                      en ? "Observe hidden pity (triggered %llu)"
                         : u8"只读观测隐藏必中保底 (已触发%llu次)",
                      static_cast<unsigned long long>(g_autoWinCount));
        return true;
    }
    if (g_lockEnabled) {
        if (en) {
            std::snprintf(outBuf, bufCap, "Locked=%s | Hint=%s | Verbose=%s (%llu rounds)",
                          FormatOrderText(g_lockOrder, g_style, true).c_str(),
                          g_hintEnabled ? "ON" : "OFF",
                          g_verbose ? "ON" : "OFF",
                          static_cast<unsigned long long>(g_roundCount));
        } else {
            std::snprintf(outBuf, bufCap, u8"已锁序=%s | 提示=%s | 详细=%s (%llu轮)",
                          FormatOrderText(g_lockOrder, g_style, false).c_str(),
                          g_hintEnabled ? u8"开" : u8"关",
                          g_verbose ? u8"开" : u8"关",
                          static_cast<unsigned long long>(g_roundCount));
        }
    } else {
        if (en) {
            std::snprintf(outBuf, bufCap, "Hints only | Style=%s | %s | Verbose=%s (%llu rounds)",
                          StyleName(g_style, true),
                          g_hintEnabled ? "ON" : "OFF",
                          g_verbose ? "ON" : "OFF",
                          static_cast<unsigned long long>(g_roundCount));
        } else {
            std::snprintf(outBuf, bufCap, u8"纯提示不改盘 | 样式=%s | %s | 详细=%s (%llu轮)",
                          StyleName(g_style, false),
                          g_hintEnabled ? u8"开" : u8"关",
                          g_verbose ? u8"开" : u8"关",
                          static_cast<unsigned long long>(g_roundCount));
        }
    }
    return true;
}

// ============================================================================
//  `$rng steam` Subcommand Handler
// ============================================================================

void PrintSteamStatus() {
    const bool en = MhwRng_HostIsEnglish(g_host);
    bool          hintEnabled = false;
    bool          notifyAuto  = false;
    bool          verbose     = false;
    bool          lockEnabled = false;
    HintStyle     style       = HintStyle::kKeyboard;
    std::array<std::uint8_t, 3> lockOrd{};
    std::uint64_t rounds = 0ULL, reds = 0ULL, autoWins = 0ULL;
    std::uint64_t permCnts[6]{};
    bool          lastValid = false, lastRed = false, last10x = false, lastAuto = false;
    std::array<std::uint8_t, 3> lastOrd{};
    std::uint32_t lastPity = 0U;
    std::uint8_t  lastStreak = 0U;
    {
        std::lock_guard<std::mutex> lk(g_stateMx);
        hintEnabled = g_hintEnabled;
        notifyAuto  = g_notifyAutoWin;
        verbose     = g_verbose;
        lockEnabled = g_lockEnabled;
        style       = g_style;
        lockOrd     = g_lockOrder;
        rounds      = g_roundCount;
        reds        = g_redCount;
        autoWins    = g_autoWinCount;
        std::memcpy(permCnts, g_permCounts, sizeof(permCnts));
        lastValid   = g_lastValid;
        lastOrd     = g_lastOrder;
        lastRed     = g_lastIsRed;
        last10x     = g_lastIs10x;
        lastAuto    = g_lastAutoWin;
        lastPity    = g_lastPityPct;
        lastStreak  = g_lastStreak;
    }

    const std::uint32_t engMode = g_host->GetEngineMode();
    const char* engNote = nullptr;
    if (en) {
        engNote = (engMode == kMhwRngModeOff)
                      ? "OFF (need `$rng selective`/`on` or `observe`)"
                      : ((engMode == kMhwRngModeObserveOnly)
                             ? "ObserveOnly"
                             : ((engMode == kMhwRngModeSelective) ? "Selective" : "Takeover"));
        g_host->ChatPrintf("[steam] Hint:%s | Verbose:%s | Style:%s | Pity:%s | Lock:%s | Kernel:%s",
                           hintEnabled ? "ON" : "OFF",
                           verbose ? "ON" : "OFF(quiet lock/pity)",
                           StyleName(style, true),
                           notifyAuto ? "ON" : "OFF",
                           lockEnabled ? FormatOrderText(lockOrd, style, true).c_str() : "OFF(vanilla)",
                           engNote);
    } else {
        engNote = (engMode == kMhwRngModeOff)
                      ? u8"框架已关闭(需开$rng selective/$rng on或observe)"
                      : ((engMode == kMhwRngModeObserveOnly)
                             ? u8"仅观测"
                             : ((engMode == kMhwRngModeSelective) ? u8"按需接管" : u8"全局接管"));
        g_host->ChatPrintf(u8"[steam] 提示:%s | 详细:%s | 样式:%s | 保底提醒:%s | 锁序:%s | 内核:%s",
                           hintEnabled ? u8"开" : u8"关",
                           verbose ? u8"开" : u8"关(锁序/保底静默)",
                           StyleName(style, false),
                           notifyAuto ? u8"开" : u8"关",
                           lockEnabled ? FormatOrderText(lockOrd, style, false).c_str() : u8"关(纯透视原版)",
                           engNote);
    }

    if (lastValid) {
        if (en) {
            g_host->ChatPrintf("[steam] Last #%llu: %s%s%s | Streak:%u | Pity:%u%% | Total:%llu (Red:%llu / Pity:%llu)",
                               static_cast<unsigned long long>(rounds),
                               FormatOrderText(lastOrd, style, true).c_str(),
                               lastRed ? "[RED]" : "",
                               lastAuto ? "[AUTO-WIN]" : (last10x ? "[10x]" : ""),
                               static_cast<unsigned>(lastStreak),
                               lastPity,
                               static_cast<unsigned long long>(rounds),
                               static_cast<unsigned long long>(reds),
                               static_cast<unsigned long long>(autoWins));
            g_host->ChatPrintf("[steam] Perms: awd=%llu adw=%llu wad=%llu wda=%llu daw=%llu dwa=%llu",
                               static_cast<unsigned long long>(permCnts[0]),
                               static_cast<unsigned long long>(permCnts[1]),
                               static_cast<unsigned long long>(permCnts[2]),
                               static_cast<unsigned long long>(permCnts[3]),
                               static_cast<unsigned long long>(permCnts[4]),
                               static_cast<unsigned long long>(permCnts[5]));
        } else {
            g_host->ChatPrintf(u8"[steam] 最近#%llu: %s%s%s | 连胜:%u | 隐藏保底率:%u%% | 统计:共%llu轮(红槽%llu/必中%llu)",
                               static_cast<unsigned long long>(rounds),
                               FormatOrderText(lastOrd, style, false).c_str(),
                               lastRed ? u8"[红槽]" : "",
                               lastAuto ? u8"[必中]" : (last10x ? u8"[10x]" : ""),
                               static_cast<unsigned>(lastStreak),
                               lastPity,
                               static_cast<unsigned long long>(rounds),
                               static_cast<unsigned long long>(reds),
                               static_cast<unsigned long long>(autoWins));
            g_host->ChatPrintf(u8"[steam] 6排列分布: awd=%llu adw=%llu wad=%llu wda=%llu daw=%llu dwa=%llu",
                               static_cast<unsigned long long>(permCnts[0]),
                               static_cast<unsigned long long>(permCnts[1]),
                               static_cast<unsigned long long>(permCnts[2]),
                               static_cast<unsigned long long>(permCnts[3]),
                               static_cast<unsigned long long>(permCnts[4]),
                               static_cast<unsigned long long>(permCnts[5]));
        }
    } else {
        g_host->ChatPrint(en ? "[steam] No Steamworks draws yet; start a round at the Steamworks"
                             : u8"[steam] 尚无蒸汽机抽签记录；在据点开启蒸汽机并按下启动键后自动提示本轮三键顺序");
    }
}

void PrintSteamHelp() {
    if (MhwRng_HostIsEnglish(g_host)) {
        g_host->ChatPrint("[steam] Hints: `$rng steam [on|off]` | `verbose [on|off]` (alias `v`, default off: quiet lock/pity)");
        g_host->ChatPrint("[steam] Pity:  `$rng steam pity [on|off]` (hidden auto-win alerts, requires `verbose on`)");
        g_host->ChatPrint("[steam] Style: `$rng steam style <kb|cn|xbox|ps|num>` (default kb: A-W-D)");
        g_host->ChatPrint("[steam] Lock:  `$rng steam lock [awd|adw|wad|wda|daw|dwa|r2|off]` (default awd for holding Space/RT/R2)");
        g_host->ChatPrint("[steam] Info:  `$rng steam status` | `$rng steam reset`");
    } else {
        g_host->ChatPrint(u8"[steam] 提示控制: `$rng steam [on|off]` | `$rng steam verbose [on|off]` (别名 `v`, 默认关: 静默锁序与保底)");
        g_host->ChatPrint(u8"[steam] 保底开关: `$rng steam pity [on|off]` (隐藏必中保底播报，需 `verbose on`)");
        g_host->ChatPrint(u8"[steam] 样式切换: `$rng steam style <kb|cn|xbox|ps|num>` (默认 kb: A-W-D (左-中-右))");
        g_host->ChatPrint(u8"[steam] 可选锁序: `$rng steam lock [awd|adw|wad|wda|daw|dwa|r2|off]` (不写参数默认A-W-D，需按需/全局接管)");
        g_host->ChatPrint(u8"[steam] 统计清理: `$rng steam status` | `$rng steam reset`");
    }
}

void __fastcall Cmd_Steam(int argc, const char* const* argv, const char*, void*) {
    if (!g_host) return;
    const bool en = MhwRng_HostIsEnglish(g_host);
    if (argc == 0) {
        PrintSteamStatus();
        return;
    }

    const std::string a0 = LowerAscii(argv[0]);
    if (a0 == "status" || a0 == "stat" || a0 == "info") { PrintSteamStatus(); return; }
    if (a0 == "help" || a0 == "?") { PrintSteamHelp(); return; }
    if (a0 == "on" || a0 == "enable") {
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_hintEnabled = true;
        }
        if (g_host->GetEngineMode() == kMhwRngModeOff) {
            g_host->ChatPrint(en ? "[steam] Live hints ON (kernel is OFF; run `$rng observe`, `$rng selective`, or `$rng on`)"
                                 : u8"[steam] 已开启蒸汽机按键实时提示 (提示: 当前框架为关闭状态，需 `$rng observe`/`$rng selective` 或 `$rng on`)");
        } else {
            g_host->ChatPrint(en ? "[steam] Live hints ON" : u8"[steam] 已开启蒸汽机按键实时提示");
        }
        return;
    }
    if (a0 == "off" || a0 == "disable") {
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_hintEnabled = false;
        }
        g_host->ChatPrint(en ? "[steam] Live hints OFF" : u8"[steam] 已关闭蒸汽机按键实时提示");
        return;
    }
    if (a0 == "reset" || a0 == "clear") {
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_lockEnabled  = false;
            g_roundCount   = 0ULL;
            g_redCount     = 0ULL;
            g_autoWinCount = 0ULL;
            std::memset(g_permCounts, 0, sizeof(g_permCounts));
            g_lastValid    = false;
            g_lastIsRed    = false;
            g_lastAutoWin  = false;
            g_lastPityPct  = 0U;
            g_lastStreak   = 0U;
        }
        g_host->ChatPrint(en ? "[steam] Cleared order lock and reset Steamworks stats"
                             : u8"[steam] 已清除锁序并重置蒸汽机统计计数");
        return;
    }
    if (a0 == "verbose" || a0 == "v") {
        if (argc < 2) {
            bool cur = false;
            { std::lock_guard<std::mutex> lk(g_stateMx); cur = g_verbose; }
            g_host->ChatPrintf(en ? "[steam] Verbose is %s (`$rng steam verbose on|off`)"
                                  : u8"[steam] 详细提示(verbose)当前: %s (开=每轮锁序与隐藏保底均播报；关=仅原版未锁序时提示 | 用法: `$rng steam verbose on|off`)",
                               cur ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"));
            return;
        }
        const std::string v = LowerAscii(argv[1]);
        const bool val = (v == "on" || v == "1" || v == "true");
        if (!val && v != "off" && v != "0" && v != "false") {
            g_host->ChatPrint(en ? "[steam] Usage: `$rng steam verbose <on|off>`" : u8"[steam] 用法: `$rng steam verbose <on|off>`");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_verbose = val;
        }
        g_host->ChatPrintf(en ? "[steam] Verbose %s" : u8"[steam] 详细提示(verbose)已%s (%s)",
                           val ? (en ? "ON (shows locked rounds & pity)" : u8"开启")
                               : (en ? "OFF (quiet when locked)" : u8"关闭"),
                           val ? (en ? "" : u8"锁序每轮与隐藏保底将播报") : (en ? "" : u8"仅保留原版未锁序时的按键提示"));
        return;
    }

    if (a0 == "pity") {
        if (argc < 2) {
            bool cur = true;
            bool curVerbose = false;
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                cur        = g_notifyAutoWin;
                curVerbose = g_verbose;
            }
            g_host->ChatPrintf(en ? "[steam] Pity alert: %s | Verbose: %s (`$rng steam pity on|off`)"
                                  : u8"[steam] 隐藏必中保底提醒当前: %s | 详细(verbose): %s (用法: `$rng steam pity on|off`)",
                               cur ? (en ? "ON" : u8"开") : (en ? "OFF" : u8"关"),
                               curVerbose ? (en ? "ON" : u8"开") : (en ? "OFF-quiet" : u8"关-静默"));
            return;
        }
        const std::string v = LowerAscii(argv[1]);
        const bool val = (v == "on" || v == "1" || v == "true");
        if (!val && v != "off" && v != "0" && v != "false") {
            g_host->ChatPrint(en ? "[steam] Usage: `$rng steam pity <on|off>`" : u8"[steam] 用法: `$rng steam pity <on|off>`");
            return;
        }
        bool curVerbose = false;
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_notifyAutoWin = val;
            curVerbose      = g_verbose;
        }
        if (val && !curVerbose) {
            g_host->ChatPrint(en ? "[steam] Pity alert ON (verbose is OFF; run `$rng steam verbose on` to show in chat)"
                                 : u8"[steam] 隐藏必中保底提醒已开启 (当前 verbose 为关，如需游戏内播报请执行 `$rng steam verbose on`)");
        } else {
            g_host->ChatPrintf(en ? "[steam] Pity alert %s" : u8"[steam] 隐藏必中保底提醒已%s",
                               val ? (en ? "ON" : u8"开启") : (en ? "OFF" : u8"关闭"));
        }
        return;
    }
    if (a0 == "style") {
        if (argc < 2) {
            g_host->ChatPrint(en ? "[steam] Usage: `$rng steam style <kb|cn|xbox|ps|num>`" : u8"[steam] 用法: `$rng steam style <kb|cn|xbox|ps|num>`");
            return;
        }
        const std::string v = LowerAscii(argv[1]);
        HintStyle st = HintStyle::kKeyboard;
        if (v == "kb" || v == "pc" || v == "awd") st = HintStyle::kKeyboard;
        else if (v == "cn" || v == "zh") st = HintStyle::kChinese;
        else if (v == "xbox" || v == "xb" || v == "xyb") st = HintStyle::kXbox;
        else if (v == "ps" || v == "ps4" || v == "ds4") st = HintStyle::kPlaySta;
        else if (v == "num" || v == "123") st = HintStyle::kNumeric;
        else {
            g_host->ChatPrint(en ? "[steam] Unknown style; choose `kb` / `cn` / `xbox` / `ps` / `num`"
                                 : u8"[steam] 未知样式；可选: `kb` / `cn` / `xbox` / `ps` / `num`");
            return;
        }
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_style = st;
        }
        g_host->ChatPrintf(en ? "[steam] Hint style -> %s" : u8"[steam] 按键提示样式已切换为: %s", StyleName(st, en));
        return;
    }
    if (a0 == "lock") {
        if (argc < 2) {
            constexpr std::array<std::uint8_t, 3> kDefaultAwd{0U, 1U, 2U};
            HintStyle curStyle   = HintStyle::kKeyboard;
            bool      curVerbose = false;
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_lockEnabled = true;
                g_lockOrder   = kDefaultAwd;
                g_lockCondId  = MakeOrderCondId(kDefaultAwd);
                curStyle      = g_style;
                curVerbose    = g_verbose;
            }
            const std::string ordTxt = FormatOrderText(kDefaultAwd, curStyle, en);
            if (!g_host->IsTakeoverEnabled()) {
                g_host->ChatPrintf(en ? "[steam] Armed default lock=%s (kernel paused; run `$rng selective` or `$rng on`%s)"
                                      : u8"[steam] 未指定顺序，已默认预设锁序=%s (按住空格/RT/R2；当前非接管模式，请先 `$rng selective` 或 `$rng on`%s)",
                                   ordTxt.c_str(),
                                   curVerbose ? "" : (en ? "" : u8"；每轮静默，要显示开 `$rng steam verbose on`"));
            } else {
                g_host->ChatPrintf(en ? "[steam] Locked key order -> %s (hold Space/RT/R2%s)"
                                      : u8"[steam] 未指定顺序，已默认锁定为 %s (按住空格/RT/R2即全中；`lock <顺序|off>`可改或解除%s)",
                                   ordTxt.c_str(),
                                   curVerbose ? "" : (en ? "; quiet, run verbose on to show" : u8"；每轮静默，要显示开 `$rng steam verbose on`"));
            }
            return;
        }
        const std::string v = LowerAscii(argv[1]);
        if (v == "off" || v == "clear" || v == "none" || v == "0") {
            {
                std::lock_guard<std::mutex> lk(g_stateMx);
                g_lockEnabled = false;
            }
            g_host->ChatPrint(en ? "[steam] Unlocked key order -> vanilla RNG (hints still active)"
                                 : u8"[steam] 已解除固定按键顺序 -> 恢复原版顺序(仅提示)");
            return;
        }
        std::array<std::uint8_t, 3> ord{};
        if (!ParsePermOrder(argv[1], ord)) {
            g_host->ChatPrint(en ? "[steam] Invalid order; specify e.g. `awd` / `wda` / `123` / `r2` or `off`"
                                 : u8"[steam] 无效顺序；请指定 3 键全排列如 `awd` / `wda` / `123` / `r2` 或 `off`");
            return;
        }
        HintStyle curStyle   = HintStyle::kKeyboard;
        bool      curVerbose = false;
        {
            std::lock_guard<std::mutex> lk(g_stateMx);
            g_lockEnabled = true;
            g_lockOrder   = ord;
            g_lockCondId  = MakeOrderCondId(ord);
            curStyle      = g_style;
            curVerbose    = g_verbose;
        }
        const std::string ordTxt = FormatOrderText(ord, curStyle, en);
        if (!g_host->IsTakeoverEnabled()) {
            g_host->ChatPrintf(en ? "[steam] Armed lock=%s (run `$rng selective` or `$rng on` to enable)"
                                  : u8"[steam] 已预设锁序=%s (注意: 当前非接管模式，请执行 `$rng selective` 或 `$rng on` 使锁序生效)%s",
                               ordTxt.c_str(),
                               en ? "" : (curVerbose ? "" : u8" (每轮静默，如需播报开 `$rng steam verbose on`)"));
        } else {
            g_host->ChatPrintf(en ? "[steam] Locked key order -> %s%s"
                                  : u8"[steam] 已锁定每轮按键顺序 -> %s%s",
                               ordTxt.c_str(),
                               curVerbose ? "" : (en ? " (quiet; `verbose on` to show)" : u8" (每轮静默，如需播报开 `$rng steam verbose on`)"));
        }
        return;
    }

    g_host->ChatPrintf(en ? "[steam] Unknown arg `%s`; type `$rng steam help`" : u8"[steam] 未知参数 `%s`；输 `$rng steam help` 查看用法", argv[0]);
}

void __fastcall Complete_Steam(const char*,
                               int argc,
                               const char* const* argv,
                               const char*,
                               MhwRngCompletionSinkFn addCand,
                               void* sinkCtx,
                               void*) {
    if (!addCand) return;
    if (argc == 0) {
        static const char* const kTop[] = {
            "status", "help", "on", "off", "verbose", "pity", "style", "lock", "reset"
        };
        for (const char* t : kTop) addCand(t, sinkCtx);
        return;
    }
    if (argc == 1 && argv && argv[0]) {
        const std::string a0 = LowerAscii(argv[0]);
        if (a0 == "verbose" || a0 == "v" || a0 == "pity") {
            addCand("on", sinkCtx);
            addCand("off", sinkCtx);
        } else if (a0 == "style") {
            static const char* const kStyles[] = { "kb", "cn", "xbox", "ps", "num" };
            for (const char* s : kStyles) addCand(s, sinkCtx);
        } else if (a0 == "lock") {
            static const char* const kOrders[] = { "awd", "adw", "wad", "wda", "daw", "dwa", "r2", "off" };
            for (const char* o : kOrders) addCand(o, sinkCtx);
        }
    }
}

const MhwRngPluginDescriptor kPluginDesc = {
    kMhwRngApiVersion,
    "steam",
    u8"蒸汽机按键提示与控序插件",
    "1.3.0",
    u8"实时解码蒸汽机三键顺序、红槽与隐藏必中保底，支持ObserveOnly纯透视与Selective/Takeover锁序"
};

} // namespace

MHW_RNG_PLUGIN_EXPORT const MhwRngPluginDescriptor* MhwRngPlugin_GetInfo() {
    return &kPluginDesc;
}

MHW_RNG_PLUGIN_EXPORT bool MhwRngPlugin_Init(const MhwRngHostApi* host, MhwRngPluginId selfId) {
    if (!host || host->apiVersion != kMhwRngApiVersion) return false;
    g_host   = host;
    g_selfId = selfId;

    MhwRng_RegisterPluginInfoBi(
        host, selfId,
        u8"蒸汽机按键提示与控序插件",
        "Steamworks Key Hint & Sequence Controller",
        u8"实时解码蒸汽机三键顺序、红槽与隐藏必中保底，支持纯透视与锁序",
        "Live Steamworks 3-key hints, red bonus/pity tracking & optional sequence lock");

    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaSteamCode1,
        "steam.code.1",
        u8"蒸汽机三键排列生成 #1 (pos1 = r1 % 3)",
        "Steamworks 3-key sequence gen #1 (pos1 = r1 % 3)",
        &FormatSteamSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaSteamCode2,
        "steam.code.2",
        u8"蒸汽机三键排列生成 #2 (pos2 = (pos1 + (r2&1) + 1) % 3)",
        "Steamworks 3-key sequence gen #2 (pos2 = (pos1 + (r2&1) + 1) % 3)",
        &FormatSteamSlotStatus,
        nullptr);
    MhwRng_RegisterSiteMetaBi(
        host, selfId,
        kRvaSteamAutoWin,
        "steam.autowin",
        u8"蒸汽机隐藏必中保底判定 (r % 100 < pityPct)",
        "Steamworks hidden guaranteed-win pity roll (r % 100 < pityPct)",
        &FormatSteamSlotStatus,
        nullptr);

    MhwRngHookDesc h1{};
    h1.structSize = sizeof(h1);
    h1.rva        = kRvaSteamCode1;
    h1.priority   = 0;
    h1.flags      = kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly;
    h1.preFn      = &OnSteamCode1Pre;
    h1.postFn     = &OnSteamCode1Post;
    g_hookCode1   = host->RegisterSlotHookEx(selfId, &h1);

    MhwRngHookDesc h2{};
    h2.structSize = sizeof(h2);
    h2.rva        = kRvaSteamCode2;
    h2.priority   = 0;
    h2.flags      = kMhwRngHookPre | kMhwRngHookPost | kMhwRngHookObserveOnly;
    h2.preFn      = &OnSteamCode2Pre;
    h2.postFn     = &OnSteamCode2Post;
    g_hookCode2   = host->RegisterSlotHookEx(selfId, &h2);

    MhwRngHookDesc hAuto{};
    hAuto.structSize = sizeof(hAuto);
    hAuto.rva        = kRvaSteamAutoWin;
    hAuto.priority   = 0;
    hAuto.flags      = kMhwRngHookPost | kMhwRngHookObserveOnly;
    hAuto.postFn     = &OnSteamAutoWinPost;
    g_hookAutoWin    = host->RegisterSlotHookEx(selfId, &hAuto);

    g_hookLife = host->RegisterLifecycleHook(selfId, &OnKernelLifecycle, nullptr);

    MhwRng_RegisterSubcommandBi(
        host, selfId,
        "steam",
        "$rng steam [on|off|verbose [on|off]|style kb|lock [awd|off]|pity on|reset]",
        u8"蒸汽机三键顺序实时提示与可选固定按键顺序",
        "Steamworks live 3-key hint overlay & optional key-order lock",
        &Cmd_Steam,
        nullptr);
    MhwRng_RegisterSubcommandCompleter(host, selfId, "steam", &Complete_Steam, nullptr);

    if (g_hookCode1 == kMhwRngInvalidHandle || g_hookCode2 == kMhwRngInvalidHandle) {
        return false;
    }
    return true;
}

MHW_RNG_PLUGIN_EXPORT void MhwRngPlugin_Shutdown() {
    std::lock_guard<std::mutex> lk(g_stateMx);
    g_hintEnabled   = true;
    g_notifyAutoWin = true;
    g_verbose       = false;
    g_lockEnabled   = false;
    g_roundCount    = 0ULL;
    g_redCount      = 0ULL;
    g_autoWinCount  = 0ULL;
    std::memset(g_permCounts, 0, sizeof(g_permCounts));
    g_lastValid     = false;
    g_hookCode1     = kMhwRngInvalidHandle;
    g_hookCode2     = kMhwRngInvalidHandle;
    g_hookAutoWin   = kMhwRngInvalidHandle;
    g_hookLife      = kMhwRngInvalidHandle;
    g_host          = nullptr;
    g_selfId        = 0U;
}
