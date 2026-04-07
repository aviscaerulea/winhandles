// vim: set ft=cpp fenc=utf-8 ff=unix ts=4 sw=4 et :
// ==================================================
// winhandles
// Windows ハンドル消費量をプロセス別に調査・レポートする CLI ツール
//
// 使用方法：
//   winhandles [--top N] [--all] [--pid PID] [--help]
//
// 依存：
//   Windows SDK（ntdll.dll、psapi.dll）
//   管理者権限推奨（SeDebugPrivilege 有効化のため）
// ==================================================

#ifndef VERSION_STR
#define VERSION_STR "dev"
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <winternl.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

// =============================================
// NT API の追加型定義（公開ヘッダにないもの）
// =============================================

// NtQuerySystemInformation の拡張ハンドル情報クラス（64）
#define SystemExtendedHandleInformation ((SYSTEM_INFORMATION_CLASS)64)

// NtQueryObject の情報クラス（winternl.h の定義を拡張）
#define ObjNameInformation ((OBJECT_INFORMATION_CLASS)1)
#define ObjTypeInformation ((OBJECT_INFORMATION_CLASS)2)

// STATUS コード（winternl.h 未定義の場合）
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

// 拡張ハンドル情報エントリ（SystemExtendedHandleInformation で返却）
typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID       Object;
    ULONG_PTR   UniqueProcessId;
    ULONG_PTR   HandleValue;
    ULONG       GrantedAccess;
    USHORT      CreatorBackTraceIndex;
    USHORT      ObjectTypeIndex;
    ULONG       HandleAttributes;
    ULONG       Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR                         NumberOfHandles;
    ULONG_PTR                         Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX;

// NtQueryObject(ObjTypeInformation) が返す型情報（先頭の TypeName だけ使用）
typedef struct _OBJECT_TYPE_INFO {
    UNICODE_STRING TypeName;
    // 以降にフィールド多数あるが TypeName のみ参照するため省略
} OBJECT_TYPE_INFO;

// =============================================
// NT API の関数ポインタ型
// =============================================
typedef NTSTATUS (NTAPI *PFN_NtQuerySystemInformation)(
    SYSTEM_INFORMATION_CLASS SystemInformationClass,
    PVOID  SystemInformation,
    ULONG  SystemInformationLength,
    PULONG ReturnLength);

typedef NTSTATUS (NTAPI *PFN_NtQueryObject)(
    HANDLE                   Handle,
    OBJECT_INFORMATION_CLASS ObjectInformationClass,
    PVOID                    ObjectInformation,
    ULONG                    ObjectInformationLength,
    PULONG                   ReturnLength);

typedef NTSTATUS (NTAPI *PFN_NtDuplicateObject)(
    HANDLE      SourceProcessHandle,
    HANDLE      SourceHandle,
    HANDLE      TargetProcessHandle,
    PHANDLE     TargetHandle,
    ACCESS_MASK DesiredAccess,
    ULONG       Attributes,
    ULONG       Options);

// =============================================
// グローバル NT API ポインタ
// =============================================
static PFN_NtQuerySystemInformation g_NtQSI = nullptr;
static PFN_NtQueryObject            g_NtQO  = nullptr;
static PFN_NtDuplicateObject        g_NtDO  = nullptr;

// =============================================
// ワイド文字列 → UTF-8 変換
// =============================================
static std::string WToU8(const std::wstring& ws)
{
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1,
                        s.data(), len, nullptr, nullptr);
    return s;
}

// =============================================
// NT API の動的ロード
// =============================================
static bool LoadNtApis()
{
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;

    g_NtQSI = reinterpret_cast<PFN_NtQuerySystemInformation>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    g_NtQO = reinterpret_cast<PFN_NtQueryObject>(
        GetProcAddress(ntdll, "NtQueryObject"));
    g_NtDO = reinterpret_cast<PFN_NtDuplicateObject>(
        GetProcAddress(ntdll, "NtDuplicateObject"));

    return g_NtQSI && g_NtQO && g_NtDO;
}

// =============================================
// SeDebugPrivilege を有効化
// =============================================
static bool EnableDebugPrivilege()
{
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;

    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, L"SeDebugPrivilege", &luid)) {
        CloseHandle(token);
        return false;
    }

    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount           = 1;
    tp.Privileges[0].Luid       = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
    bool ok = (GetLastError() == ERROR_SUCCESS);
    CloseHandle(token);
    return ok;
}

// =============================================
// NtQueryObject(ObjNameInformation) タイムアウト付き安全呼び出し
//
// 名前付きパイプ・ALPC ポートなどで NtQueryObject がデッドロックする
// ことがあるため、ワーカースレッド + タイムアウトで保護する。
// タイムアウト時は TerminateThread し、ctx はリーク（解放するとクラッシュ
// する可能性があるため）。
// =============================================
struct QueryNameCtx {
    HANDLE       handle;
    std::wstring result;
    volatile bool done;
};

static DWORD WINAPI QueryNameWorker(LPVOID param)
{
    auto* ctx = reinterpret_cast<QueryNameCtx*>(param);

    // 4096 バイト（約 2000 文字）で通常の NT パスを網羅する。
    // 超長パス（32767 文字等）はサイレントに空文字列になる（タイムアウト時の
    // ヒープ破壊リスクを避けるためスタック固定サイズとする）
    BYTE     buf[4096];
    ULONG    ret;
    NTSTATUS st = g_NtQO(ctx->handle, ObjNameInformation,
                         buf, sizeof(buf), &ret);
    if (NT_SUCCESS(st)) {
        auto* uni = reinterpret_cast<UNICODE_STRING*>(buf);
        if (uni->Buffer && uni->Length > 0)
            ctx->result.assign(uni->Buffer, uni->Length / sizeof(WCHAR));
    }
    ctx->done = true;
    return 0;
}

static std::wstring SafeQueryObjectName(HANDLE h, DWORD timeoutMs = 150)
{
    // スレッドがタイムアウト後も ctx を参照し続ける可能性があるためヒープ割り当て
    auto* ctx = new QueryNameCtx{h, L"", false};

    HANDLE thr = CreateThread(nullptr, 0, QueryNameWorker, ctx, 0, nullptr);
    if (!thr) {
        delete ctx;
        return L"";
    }

    if (WaitForSingleObject(thr, timeoutMs) == WAIT_TIMEOUT) {
        TerminateThread(thr, 0);
        CloseHandle(thr);
        // ctx はリーク（TerminateThread 後の解放はクラッシュリスクのため）
        return L"<timeout>";
    }

    std::wstring result = ctx->result;
    CloseHandle(thr);
    delete ctx;
    return result;
}

// =============================================
// 型インデックス → 型名キャッシュ
// =============================================
using TypeNameCache = std::unordered_map<USHORT, std::wstring>;

// 複製済みハンドルから型名を解決してキャッシュに登録する
static std::wstring ResolveTypeName(HANDLE dupHandle, TypeNameCache& cache, USHORT typeIdx)
{
    auto it = cache.find(typeIdx);
    if (it != cache.end()) return it->second;

    BYTE     buf[4096];
    ULONG    ret;
    NTSTATUS st = g_NtQO(dupHandle, ObjTypeInformation,
                         buf, sizeof(buf), &ret);

    std::wstring name;
    if (NT_SUCCESS(st)) {
        auto* ti = reinterpret_cast<OBJECT_TYPE_INFO*>(buf);
        if (ti->TypeName.Buffer && ti->TypeName.Length > 0)
            name.assign(ti->TypeName.Buffer, ti->TypeName.Length / sizeof(WCHAR));
    }
    if (name.empty()) name = L"Unknown";

    cache[typeIdx] = name;
    return name;
}

// =============================================
// プロセスごとの集計データ
// =============================================
struct ProcessInfo {
    DWORD        pid;
    std::wstring name;
    ULONG        totalHandles;
    std::map<std::wstring, ULONG> typeCount; // 型名 → ハンドル数
};

// =============================================
// プロセス名の取得（ベースファイル名）
// =============================================
static std::wstring GetProcessName(DWORD pid)
{
    if (pid == 0) return L"Idle";
    if (pid == 4) return L"System";

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return L"<access denied>";

    WCHAR  path[MAX_PATH] = {};
    DWORD  len = MAX_PATH;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        CloseHandle(proc);
        std::wstring full(path, len);
        auto pos = full.rfind(L'\\');
        return (pos != std::wstring::npos) ? full.substr(pos + 1) : full;
    }

    CloseHandle(proc);
    return L"<unknown>";
}

// =============================================
// 全ハンドルの列挙（バッファを自動拡張）
// =============================================
static std::vector<BYTE> EnumerateHandles()
{
    std::vector<BYTE> buf(4 * 1024 * 1024); // 4MB から開始
    ULONG    ret;
    NTSTATUS st;

    // 256MB を上限として 2 倍ずつ拡張する
    const size_t MAX_BUF = 256ULL * 1024 * 1024;

    for (;;) {
        st = g_NtQSI(SystemExtendedHandleInformation,
                     buf.data(), static_cast<ULONG>(buf.size()), &ret);
        if (st == STATUS_INFO_LENGTH_MISMATCH) {
            if (buf.size() >= MAX_BUF) break;
            buf.resize(buf.size() * 2);
            continue;
        }
        break;
    }

    if (!NT_SUCCESS(st)) buf.clear();
    return buf;
}

// =============================================
// 数値を k/m 単位に変換
// =============================================
static std::string FormatCount(ULONG n)
{
    std::ostringstream ss;
    if (n >= 1000000)
        ss << std::fixed << std::setprecision(1) << n / 1000000.0 << "m";
    else if (n >= 1000)
        ss << static_cast<ULONG>(std::round(n / 1000.0)) << "k";
    else
        ss << n;
    return ss.str();
}

// =============================================
// トップ N レポートの出力
// =============================================
static void PrintReport(const std::vector<ProcessInfo>& procs,
                         ULONG_PTR totalHandles,
                         size_t    topN,
                         bool      showAll)
{
    size_t limit = showAll ? procs.size() : std::min(topN, procs.size());

    std::cout << "\nハンドルレポート（合計：" << totalHandles
              << " / " << procs.size() << " プロセス）\n";
    std::cout << std::string(72, '=') << "\n";
    std::cout << std::right << std::setw(6)  << "PID"
              << "  " << std::left  << std::setw(28) << "プロセス名"
              << std::right << std::setw(10) << "ハンドル数"
              << "  主要な型\n";
    std::cout << std::string(6,  '-') << "  "
              << std::string(28, '-') << "  "
              << std::string(10, '-') << "  "
              << std::string(24, '-') << "\n";

    for (size_t i = 0; i < limit; ++i) {
        const auto& p = procs[i];

        // 型別上位 3 件を文字列化
        std::vector<std::pair<ULONG, std::wstring>> types;
        for (auto& kv : p.typeCount)
            types.emplace_back(kv.second, kv.first);
        std::sort(types.begin(), types.end(), std::greater<>());

        std::ostringstream typeStr;
        for (size_t t = 0; t < std::min<size_t>(3, types.size()); ++t) {
            if (t > 0) typeStr << " ";
            typeStr << WToU8(types[t].second) << "(" << FormatCount(types[t].first) << ")";
        }

        // setw はバイト数ではなく文字数でパディングするため、日本語プロセス名が
        // ある場合は列揃えが崩れる。実用上プロセス名は ASCII のみのため許容する
        std::string name8 = WToU8(p.name.substr(0, 28));
        std::cout << std::right << std::setw(6)  << p.pid
                  << "  " << std::left  << std::setw(28) << name8
                  << std::right << std::setw(10) << p.totalHandles
                  << "  " << typeStr.str() << "\n";
    }
    std::cout << "\n";
}

// =============================================
// --pid モード：指定プロセスの詳細出力
//
// ハンドルを 1 件ずつ複製してオブジェクト名を取得する。
// 名前付きパイプ等でのデッドロックは SafeQueryObjectName で保護。
// =============================================
static void PrintProcessDetail(DWORD targetPid,
                                const SYSTEM_HANDLE_INFORMATION_EX* info,
                                TypeNameCache& typeCache)
{
    HANDLE srcProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, targetPid);
    if (!srcProc) {
        std::cerr << "プロセス " << targetPid
                  << " を開けなかった（管理者権限が必要）\n";
        return;
    }

    std::wstring procName = GetProcessName(targetPid);

    // 型名 → [オブジェクト名, ...] のマッピング
    std::map<std::wstring, std::vector<std::wstring>> typeToNames;

    std::cerr << "  ハンドル名を解析中（時間がかかる場合があります）...\n";

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        if (static_cast<DWORD>(entry.UniqueProcessId) != targetPid) continue;

        HANDLE dup = nullptr;
        NTSTATUS st = g_NtDO(srcProc,
                              reinterpret_cast<HANDLE>(entry.HandleValue),
                              GetCurrentProcess(),
                              &dup, 0, 0, DUPLICATE_SAME_ACCESS);
        if (!NT_SUCCESS(st)) continue;

        std::wstring typeName = ResolveTypeName(dup, typeCache, entry.ObjectTypeIndex);
        std::wstring objName  = SafeQueryObjectName(dup);
        CloseHandle(dup);

        typeToNames[typeName].push_back(objName);
    }

    CloseHandle(srcProc);

    // 出力
    std::cout << "\n[PID " << targetPid << " : " << WToU8(procName) << " の詳細]\n";
    std::cout << std::string(60, '=') << "\n";

    // ハンドル数降順でソート
    std::vector<std::pair<size_t, std::wstring>> sorted;
    for (auto& kv : typeToNames)
        sorted.emplace_back(kv.second.size(), kv.first);
    std::sort(sorted.begin(), sorted.end(), std::greater<>());

    for (auto& [cnt, type] : sorted) {
        std::cout << "\n  " << WToU8(type) << "  （" << cnt << " 件）\n";

        // オブジェクト名のユニーク集計（上位 10 件表示）
        std::map<std::wstring, int> nameCount;
        for (auto& n : typeToNames[type]) {
            if (!n.empty() && n != L"<timeout>")
                nameCount[n]++;
        }

        std::vector<std::pair<int, std::wstring>> namesSorted;
        for (auto& kv : nameCount)
            namesSorted.emplace_back(kv.second, kv.first);
        std::sort(namesSorted.begin(), namesSorted.end(), std::greater<>());

        size_t shown = 0;
        for (auto& [nc, name] : namesSorted) {
            if (shown >= 10) {
                std::cout << "    ...（" << (namesSorted.size() - 10)
                          << " 件省略）\n";
                break;
            }
            std::cout << "    " << std::right << std::setw(4) << nc
                      << "x  " << WToU8(name) << "\n";
            ++shown;
        }
    }
    std::cout << "\n";
}

// =============================================
// System プロセス（PID 4）ドライバ別内訳の表示
//
// File ハンドルのオブジェクト名を解析し、\Device\xxx 形式の
// パスプレフィックスでグループ化して件数を表示する。
// サンプル数を MAX_SAMPLE に制限して実行時間を抑える。
// =============================================
static void PrintSystemDriverBreakdown(
    const SYSTEM_HANDLE_INFORMATION_EX* info,
    TypeNameCache& typeCache)
{
    const ULONG MAX_SAMPLE = 2000;

    std::cout << "\n[System プロセス（PID 4）File ハンドル パス別内訳]\n";
    std::cout << std::string(60, '=') << "\n";

    HANDLE sysProc = OpenProcess(PROCESS_DUP_HANDLE, FALSE, 4);
    if (!sysProc) {
        std::cerr << "  System プロセスを開けなかった（管理者権限が必要）\n";
        return;
    }

    std::map<std::wstring, ULONG> prefixCount;
    ULONG sampled = 0;
    ULONG skipped = 0;

    std::cerr << "  System ハンドルを解析中（最大 " << MAX_SAMPLE << " 件）...\n";

    for (ULONG_PTR i = 0; i < info->NumberOfHandles && sampled < MAX_SAMPLE; ++i) {
        const auto& entry = info->Handles[i];
        if (entry.UniqueProcessId != 4) continue;

        HANDLE dup = nullptr;
        NTSTATUS st = g_NtDO(sysProc,
                              reinterpret_cast<HANDLE>(entry.HandleValue),
                              GetCurrentProcess(),
                              &dup, 0, 0, DUPLICATE_SAME_ACCESS);
        if (!NT_SUCCESS(st)) {
            ++skipped;
            continue;
        }

        std::wstring typeName = ResolveTypeName(dup, typeCache, entry.ObjectTypeIndex);

        // File 型のみオブジェクト名を解析
        if (typeName != L"File") {
            CloseHandle(dup);
            continue;
        }

        std::wstring name = SafeQueryObjectName(dup);
        CloseHandle(dup);
        ++sampled;

        if (name.empty() || name == L"<timeout>") {
            prefixCount[L"<名前なし・タイムアウト>"]++;
            continue;
        }

        // \Device\HarddiskVolume3\... → \Device\HarddiskVolume3 を抽出
        std::wstring prefix;
        size_t p1 = name.find(L'\\', 1);       // 2 番目の '\'
        if (p1 != std::wstring::npos) {
            size_t p2 = name.find(L'\\', p1 + 1); // 3 番目の '\'
            prefix = (p2 != std::wstring::npos)
                ? name.substr(0, p2)
                : name.substr(0, p1);
        }
        else {
            prefix = name;
        }
        prefixCount[prefix]++;
    }

    CloseHandle(sysProc);

    // 降順ソートして出力
    std::vector<std::pair<ULONG, std::wstring>> sorted;
    for (auto& kv : prefixCount)
        sorted.emplace_back(kv.second, kv.first);
    std::sort(sorted.begin(), sorted.end(), std::greater<>());

    std::cout << "\n  （File ハンドル " << sampled << " 件サンプル、"
              << skipped << " 件スキップ）\n\n";
    std::cout << std::right << std::setw(8) << "件数"
              << "  パスプレフィックス\n";
    std::cout << std::string(8, '-') << "  "
              << std::string(50, '-') << "\n";

    for (auto& [cnt, prefix] : sorted) {
        std::cout << std::right << std::setw(8) << cnt
                  << "  " << WToU8(prefix) << "\n";
    }
    std::cout << "\n";
}

// =============================================
// ヘルプ表示
// =============================================
static void PrintHelp()
{
    std::cout << "\nwinhandles v" VERSION_STR "\n"
                 "Windows ハンドル消費量をプロセス別に調査するツール\n\n"
                 "使用方法：\n"
                 "  winhandles [オプション]\n\n"
                 "オプション：\n"
                 "  --top N    上位 N プロセスを表示（デフォルト：20）\n"
                 "  --all      全プロセスを表示\n"
                 "  --pid PID  指定 PID のハンドル詳細を表示\n"
                 "             （PID 4 指定時はドライバ別内訳も表示）\n"
                 "  --help     このヘルプを表示\n\n"
                 "注意：正確な結果を得るには管理者権限で実行してください。\n\n";
}

// =============================================
// エントリポイント
// =============================================
int main(int argc, char* argv[])
{
    // UTF-8 出力設定（コンソール・パイプどちらも正しく出力される）
    SetConsoleOutputCP(CP_UTF8);

    // --- CLI 引数解析 ---
    size_t topN    = 20;
    bool   showAll = false;
    DWORD  pidMode = 0;

    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        if (arg == "--help" || arg == "-h") {
            PrintHelp();
            return 0;
        }
        else if (arg == "--all") {
            showAll = true;
        }
        else if (arg == "--top" && i + 1 < argc) {
            try { topN = static_cast<size_t>(std::stoul(argv[++i])); }
            catch (...) { std::cerr << "--top の引数が無効だ\n"; return 1; }
        }
        else if (arg == "--pid" && i + 1 < argc) {
            try { pidMode = static_cast<DWORD>(std::stoul(argv[++i])); }
            catch (...) { std::cerr << "--pid の引数が無効だ\n"; return 1; }
        }
        else {
            std::cerr << "不明なオプション：" << arg
                      << "（--help で使用方法を確認）\n";
            return 1;
        }
    }

    // --- NT API ロード ---
    if (!LoadNtApis()) {
        std::cerr << "エラー：ntdll.dll の API ロードに失敗した\n";
        return 1;
    }

    // --- SeDebugPrivilege 有効化 ---
    if (!EnableDebugPrivilege())
        std::cerr << "警告：SeDebugPrivilege の有効化に失敗した"
                     "（一部プロセスのハンドルを取得できない可能性がある）\n";

    // --- 全ハンドル列挙 ---
    std::cerr << "ハンドルを列挙中...\n";
    auto buf = EnumerateHandles();
    if (buf.empty()) {
        std::cerr << "エラー：ハンドル列挙に失敗した\n";
        return 1;
    }

    auto* info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX*>(buf.data());

    // --- 型名キャッシュ構築 ---
    // 各 ObjectTypeIndex について代表ハンドルを 1 件だけ複製して型名を取得
    std::cerr << "型名を解決中...\n";
    TypeNameCache typeCache;
    std::unordered_map<USHORT, bool> typeSeen;

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        if (typeSeen.count(entry.ObjectTypeIndex)) continue;

        HANDLE dup     = nullptr;
        bool   ownProc = (entry.UniqueProcessId ==
                          static_cast<ULONG_PTR>(GetCurrentProcessId()));

        if (ownProc) {
            dup = reinterpret_cast<HANDLE>(entry.HandleValue);
        }
        else {
            HANDLE src = OpenProcess(PROCESS_DUP_HANDLE, FALSE,
                                     static_cast<DWORD>(entry.UniqueProcessId));
            if (!src) continue;
            NTSTATUS st = g_NtDO(src,
                                  reinterpret_cast<HANDLE>(entry.HandleValue),
                                  GetCurrentProcess(),
                                  &dup, 0, 0, DUPLICATE_SAME_ACCESS);
            CloseHandle(src);
            if (!NT_SUCCESS(st)) continue;
        }

        ResolveTypeName(dup, typeCache, entry.ObjectTypeIndex);
        typeSeen[entry.ObjectTypeIndex] = true;

        if (!ownProc) {
            CloseHandle(dup);
            dup = nullptr;
        }
    }

    // --- PID ごとのハンドル数・型別集計 ---
    std::unordered_map<DWORD, ProcessInfo> pidMap;

    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const auto& entry = info->Handles[i];
        DWORD pid = static_cast<DWORD>(entry.UniqueProcessId);

        auto& pi = pidMap[pid];
        pi.pid = pid;
        pi.totalHandles++;

        auto it = typeCache.find(entry.ObjectTypeIndex);
        std::wstring tn = (it != typeCache.end()) ? it->second : L"Unknown";
        pi.typeCount[tn]++;
    }

    // プロセス名の解決
    for (auto& [pid, pi] : pidMap)
        pi.name = GetProcessName(pid);

    // ハンドル数降順でソート
    std::vector<ProcessInfo> sorted;
    sorted.reserve(pidMap.size());
    for (auto& [pid, pi] : pidMap)
        sorted.push_back(pi);
    std::sort(sorted.begin(), sorted.end(),
              [](const ProcessInfo& a, const ProcessInfo& b) {
                  return a.totalHandles > b.totalHandles;
              });

    // --- 出力 ---
    if (pidMode > 0) {
        PrintProcessDetail(pidMode, info, typeCache);
        if (pidMode == 4)
            PrintSystemDriverBreakdown(info, typeCache);
    }
    else {
        PrintReport(sorted, info->NumberOfHandles, topN, showAll);
    }

    return 0;
}
