#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <dwmapi.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <iostream>
#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sstream>
#include <algorithm>
#include <iomanip>
#include <atomic>
#include <mutex>
#include <cwctype>
#include <cctype>
#include <wrl.h>
#include <wil/com.h>
#include "WebView2.h"
#include <map>
#include <nlohmann/json.hpp>
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "version.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "shell32.lib")

using namespace Microsoft::WRL;
namespace fs = std::filesystem;
using json = nlohmann::json;

#define WM_WEBVIEW_UPDATE (WM_USER + 101)

// =====================================================================================
//  НАСТРОЙКИ, КОТОРЫЕ НУЖНО ЗАПОЛНИТЬ ОДИН РАЗ (см. SETUP.md)
// =====================================================================================
//  1) API_HOST  - адрес твоего Cloudflare Worker (без https://), например dune-api.name.workers.dev
//  2) PUB_X/PUB_Y - публичный ключ подписи (выдаёт server/tools/setup-keys.mjs), по 64 hex-символа
//  В exe больше НЕТ ни GitHub-ссылок, ни списка ключей: лаунчер знает только адрес твоего API.
// =====================================================================================

// --- простая обфускация строк (XOR на этапе компиляции): `strings`/hex-редактор не покажут host и ключ ---
template <size_t N> struct ObfStr {
    char d[N];
    constexpr ObfStr(const char(&s)[N]) : d{} { for (size_t i = 0; i < N; i++) d[i] = (char)(s[i] ^ 0x5A ^ (char)(i * 7 + 3)); }
    std::string get() const { std::string r; for (size_t i = 0; i + 1 < N; i++) r += (char)(d[i] ^ 0x5A ^ (char)(i * 7 + 3)); return r; }
};
#define OBF(s) ([]() { static constexpr ObfStr<sizeof(s)> o(s); return o.get(); }())


static const wchar_t* SITE_URL = L"https://dunevisualss.web.app";
static const char* LAUNCHER_VER = "2.0";static std::string ApiHost() { return OBF("dune-api.dune-api67.workers.dev"); }
static std::string PubX()    { return OBF("1dd03efa129179f15b859a02a23237b48e8cdd1d953dd5cac94583bf31570460"); }
static std::string PubY()    { return OBF("63ce7ebc9e9ec8aaf4f25b5aac3f161611a34c2214e7fbe7b221a951b0e82232"); }

std::wstring CHEAT_NAME = L"Dune Visuals";
const std::wstring MC_VER = L"1.21.11";
const std::wstring VERSION_NAME = L"Fabric 1.21.11";
const std::wstring MOD_FILENAME = L"dune.jar";   // имя на диске всегда одно, апдейт просто перезаписывает файл

HWND g_hWnd = nullptr;
wil::com_ptr<ICoreWebView2Environment> g_env;
wil::com_ptr<ICoreWebView2Controller> g_webviewController;
wil::com_ptr<ICoreWebView2> g_webview;
std::atomic<DWORD> g_GamePID{ 0 };
std::atomic<bool> g_CancelDownload{ false };
std::atomic<bool> g_Busy{ false };

const int WIN_W = 640, WIN_H = 420;

// =====================================================================================
//  Утилиты
// =====================================================================================
std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return ""; int s = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), 0, 0, 0, 0);
    std::string r(s, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &r[0], s, 0, 0); return r;
}
std::wstring Utf8ToWide(const std::string& u) {
    if (u.empty()) return L""; int s = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), 0, 0);
    std::wstring r(s, 0); MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), &r[0], s); return r;
}
std::wstring GetBaseDir() { return L"C:\\" + CHEAT_NAME + L"\\"; }
std::wstring GetMinecraftDir() { return GetBaseDir() + L".minecraft\\"; }
std::wstring GetModsDir() { return GetMinecraftDir() + L"mods\\"; }
std::wstring GetVersionDir() { return GetMinecraftDir() + L"versions\\" + VERSION_NAME + L"\\"; }

// сообщения в UI: всегда через nlohmann (никакого ручного экранирования)
void SafePostJson(const json& j) {
    if (g_hWnd) { std::wstring* m = new std::wstring(Utf8ToWide(j.dump())); PostMessage(g_hWnd, WM_WEBVIEW_UPDATE, 0, (LPARAM)m); }
}
void SendProgress(double percent, double curMb, double totMb, const std::string& statusKey) {
    std::ostringstream c, t; c.imbue(std::locale::classic()); t.imbue(std::locale::classic());
    c << std::fixed << std::setprecision(1) << curMb << "MB"; t << std::fixed << std::setprecision(1) << totMb << "MB";
    SafePostJson({ {"type","progress"},{"percent",percent},{"current",c.str()},{"total",t.str()},{"status",statusKey} });
}
void SendError(const std::string& msg) { SafePostJson({ {"type","error"},{"message",msg} }); }
void SendError(const std::wstring& msg) { SendError(WideToUtf8(msg)); }
void SendType(const char* t) { json j; j["type"] = t; SafePostJson(j); }

// =====================================================================================
//  Крипто: SHA-256, base64, hex, random, проверка подписи ECDSA P-256
// =====================================================================================
std::string ToHex(const BYTE* p, size_t n) { static const char* h = "0123456789abcdef"; std::string s; for (size_t i = 0; i < n; i++) { s += h[p[i] >> 4]; s += h[p[i] & 15]; } return s; }
bool FromHex(const std::string& s, std::vector<BYTE>& out) {
    if (s.size() % 2) return false; out.clear();
    for (size_t i = 0; i < s.size(); i += 2) { if (!isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) return false; out.push_back((BYTE)std::stoi(s.substr(i, 2), nullptr, 16)); }
    return true;
}
bool Base64Decode(const std::string& s, std::vector<BYTE>& out) {
    DWORD n = 0; if (!CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, NULL, &n, NULL, NULL) || !n) return false;
    out.resize(n); return CryptStringToBinaryA(s.c_str(), (DWORD)s.size(), CRYPT_STRING_BASE64, out.data(), &n, NULL, NULL) != 0;
}
std::string RandomHex(size_t bytes) { std::vector<BYTE> b(bytes); BCryptGenRandom(NULL, b.data(), (ULONG)bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return ToHex(b.data(), bytes); }

struct Sha256Ctx {
    BCRYPT_ALG_HANDLE a = NULL; BCRYPT_HASH_HANDLE h = NULL; bool ok = false;
    Sha256Ctx() { ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0)) && BCRYPT_SUCCESS(BCryptCreateHash(a, &h, NULL, 0, NULL, 0, 0)); }
    ~Sha256Ctx() { if (h) BCryptDestroyHash(h); if (a) BCryptCloseAlgorithmProvider(a, 0); }
    void Update(const void* p, size_t n) { if (ok) BCryptHashData(h, (PUCHAR)p, (ULONG)n, 0); }
    std::vector<BYTE> Final() { std::vector<BYTE> d(32); if (ok) BCryptFinishHash(h, d.data(), 32, 0); return d; }
};
std::string Sha256Hex(const std::string& s) { Sha256Ctx c; c.Update(s.data(), s.size()); auto d = c.Final(); return ToHex(d.data(), d.size()); }
std::string Sha256FileHex(const std::wstring& path) {
    std::ifstream f(path, std::ios::binary); if (!f) return ""; Sha256Ctx c; std::vector<char> buf(1 << 16);
    while (f) { f.read(buf.data(), buf.size()); std::streamsize n = f.gcount(); if (n > 0) c.Update(buf.data(), (size_t)n); }
    auto d = c.Final(); return ToHex(d.data(), d.size());
}

// подпись манифеста: ECDSA P-256 / SHA-256, подпись в формате r||s (64 байта) - как отдаёт WebCrypto
bool VerifyManifestSignature(const std::string& manifest, const std::string& sigB64) {
    std::vector<BYTE> x, y, sig;
    if (!FromHex(PubX(), x) || !FromHex(PubY(), y) || x.size() != 32 || y.size() != 32) return false;
    if (!Base64Decode(sigB64, sig) || sig.size() != 64) return false;
    bool zero = std::all_of(x.begin(), x.end(), [](BYTE b) { return b == 0; });
    if (zero) return false; // публичный ключ не вписан в сборку
    std::vector<BYTE> blob(sizeof(BCRYPT_ECCKEY_BLOB) + 64);
    BCRYPT_ECCKEY_BLOB* hdr = (BCRYPT_ECCKEY_BLOB*)blob.data(); hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC; hdr->cbKey = 32;
    memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), x.data(), 32); memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB) + 32, y.data(), 32);
    BCRYPT_ALG_HANDLE alg = NULL; BCRYPT_KEY_HANDLE key = NULL; bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, NULL, 0)) &&
        BCRYPT_SUCCESS(BCryptImportKeyPair(alg, NULL, BCRYPT_ECCPUBLIC_BLOB, &key, blob.data(), (ULONG)blob.size(), 0))) {
        Sha256Ctx c; c.Update(manifest.data(), manifest.size()); auto d = c.Final();
        ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, NULL, d.data(), (ULONG)d.size(), sig.data(), (ULONG)sig.size(), 0));
    }
    if (key) BCryptDestroyKey(key); if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// HWID: хэш MachineGuid + серийник системного тома (ключ привязывается к ПК при первом входе)
std::string GetHwid() {
    char guid[128] = { 0 }; DWORD sz = sizeof(guid); HKEY hk;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", 0, KEY_READ | KEY_WOW64_64KEY, &hk) == ERROR_SUCCESS) {
        RegQueryValueExA(hk, "MachineGuid", NULL, NULL, (LPBYTE)guid, &sz); RegCloseKey(hk);
    }
    DWORD serial = 0; GetVolumeInformationW(L"C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);
    return Sha256Hex(std::string(guid) + "|" + std::to_string(serial) + "|dune");
}

// =====================================================================================
//  Настройки пользователя: шифруются DPAPI (привязаны к Windows-пользователю), а не зашитым в exe AES-ключом
// =====================================================================================
struct Prefs {
    int ram = 4028; bool installed = false; bool dark = true; bool ru = true; bool hasPrefs = false;
    std::wstring nick = L"Player"; std::wstring modTag; std::wstring mcVer;
    std::string key; bool skipped = false;
};
Prefs g_P; std::mutex g_Pm; 
// текущий доступ: none | default | beta (доступ из разных потоков - через мьютекс)
std::mutex g_TierM; std::string g_TierV = "none";
std::string GetTier() { std::lock_guard<std::mutex> l(g_TierM); return g_TierV; }
void SetTier(const std::string& t) { std::lock_guard<std::mutex> l(g_TierM); g_TierV = t; }

static const BYTE DPAPI_ENTROPY[] = { 0x44,0x75,0x6E,0x65,0x21,0x9B,0x33,0xC1,0x7A,0x05 };

std::string CleanKey(const std::string& k) { std::string r; for (char c : k) if (isalnum((unsigned char)c) || c == '-' || c == '_') r += c; return r.substr(0, 64); }

void SavePrefs() {
    std::lock_guard<std::mutex> lk(g_Pm);
    HKEY hKey; std::wstring regPath = L"SOFTWARE\\" + CHEAT_NAME;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, NULL, REG_OPTION_NON_VOLATILE, KEY_WRITE, NULL, &hKey, NULL) != ERROR_SUCCESS) return;
    json j = { {"ram",g_P.ram},{"installed",g_P.installed},{"dark",g_P.dark},{"ru",g_P.ru},{"nick",WideToUtf8(g_P.nick)},
               {"modTag",WideToUtf8(g_P.modTag)},{"mcVer",WideToUtf8(g_P.mcVer)},{"key",g_P.key},{"skipped",g_P.skipped} };
    std::string s = j.dump(); DATA_BLOB in = { (DWORD)s.size(), (BYTE*)s.data() }, ent = { (DWORD)sizeof(DPAPI_ENTROPY), (BYTE*)DPAPI_ENTROPY }, out = {};
    if (CryptProtectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        RegSetValueExW(hKey, L"State", 0, REG_BINARY, out.pbData, out.cbData); LocalFree(out.pbData);
    }
    RegCloseKey(hKey);
}

void LoadPrefs() {
    std::lock_guard<std::mutex> lk(g_Pm);
    LANGID lid = PRIMARYLANGID(GetUserDefaultUILanguage()); g_P.ru = (lid == LANG_RUSSIAN || lid == LANG_UKRAINIAN || lid == LANG_BELARUSIAN);
    HKEY hKey; std::wstring regPath = L"SOFTWARE\\" + CHEAT_NAME;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, regPath.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return;
    DWORD sz = 0;
    if (RegQueryValueExW(hKey, L"State", NULL, NULL, NULL, &sz) == ERROR_SUCCESS && sz > 0) {
        std::vector<BYTE> buf(sz);
        if (RegQueryValueExW(hKey, L"State", NULL, NULL, buf.data(), &sz) == ERROR_SUCCESS) {
            DATA_BLOB in = { sz, buf.data() }, ent = { (DWORD)sizeof(DPAPI_ENTROPY), (BYTE*)DPAPI_ENTROPY }, out = {};
            if (CryptUnprotectData(&in, NULL, &ent, NULL, NULL, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                json j = json::parse(std::string((char*)out.pbData, out.cbData), nullptr, false); LocalFree(out.pbData);
                if (j.is_object()) {
                    g_P.ram = std::clamp(j.value("ram", 4028), 1024, 65536); g_P.installed = j.value("installed", false);
                    g_P.dark = j.value("dark", true); g_P.ru = j.value("ru", g_P.ru); g_P.nick = Utf8ToWide(j.value("nick", std::string("Player")));
                    g_P.modTag = Utf8ToWide(j.value("modTag", std::string())); g_P.mcVer = Utf8ToWide(j.value("mcVer", std::string()));
                    g_P.key = CleanKey(j.value("key", std::string())); g_P.skipped = j.value("skipped", false); g_P.hasPrefs = true;
                }
            }
        }
    }
    RegCloseKey(hKey);
}

// =====================================================================================
//  Сеть
// =====================================================================================
bool HttpPostJson(const std::string& host, const std::wstring& path, const std::string& body, std::string& out, DWORD& status) {
    out.clear(); status = 0;
    HINTERNET hI = InternetOpenW(L"DuneLauncher/2.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0); if (!hI) return false;
    DWORD to = 10000; InternetSetOptionW(hI, INTERNET_OPTION_CONNECT_TIMEOUT, &to, sizeof(to)); InternetSetOptionW(hI, INTERNET_OPTION_RECEIVE_TIMEOUT, &to, sizeof(to));
    HINTERNET hC = InternetConnectW(hI, Utf8ToWide(host).c_str(), INTERNET_DEFAULT_HTTPS_PORT, NULL, NULL, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hC) { InternetCloseHandle(hI); return false; }
    const wchar_t* acc[] = { L"application/json", NULL };
    HINTERNET hR = HttpOpenRequestW(hC, L"POST", path.c_str(), NULL, NULL, acc, INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI, 0);
    bool ok = false;
    if (hR) {
        std::wstring hdr = L"Content-Type: application/json\r\n";
        if (HttpSendRequestW(hR, hdr.c_str(), (DWORD)hdr.size(), (LPVOID)body.data(), (DWORD)body.size())) {
            DWORD sl = sizeof(status); HttpQueryInfoW(hR, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &sl, NULL);
            char buf[8192]; DWORD br; while (InternetReadFile(hR, buf, sizeof(buf), &br) && br > 0) out.append(buf, br);
            ok = true;
        }
        InternetCloseHandle(hR);
    }
    InternetCloseHandle(hC); InternetCloseHandle(hI); return ok;
}

// скачивание: проверка HTTP 200, размера и (если дан) SHA-256
bool DownloadFile(const std::string& url, const std::wstring& destPath, const std::string& statusKey, const std::string& expectSha = "", long long fallbackSize = 0) {
    HINTERNET hI = InternetOpenW(L"DuneLauncher/2.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!hI) { SendError("InternetOpen failed"); return false; }
    HINTERNET hU = InternetOpenUrlW(hI, Utf8ToWide(url).c_str(), NULL, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_UI | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hU) { SendError("InternetOpenUrl failed: " + std::to_string(GetLastError())); InternetCloseHandle(hI); return false; }
    DWORD code = 0, cl = sizeof(code); HttpQueryInfoW(hU, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &cl, NULL);
    if (code != 200) { SendError("HTTP " + std::to_string(code)); InternetCloseHandle(hU); InternetCloseHandle(hI); return false; }
    DWORD ts = 0, ls = sizeof(ts); HttpQueryInfoW(hU, HTTP_QUERY_CONTENT_LENGTH | HTTP_QUERY_FLAG_NUMBER, &ts, &ls, NULL);
    double total = ts ? (double)ts : (double)fallbackSize;
    std::error_code ec; fs::create_directories(fs::path(destPath).parent_path(), ec);
    std::ofstream of(destPath, std::ios::binary);
    if (!of) { SendError(L"Cannot create: " + destPath); InternetCloseHandle(hU); InternetCloseHandle(hI); return false; }
    char buf[16384]; DWORD br; unsigned long long tr = 0; DWORD lastTick = 0;
    while (InternetReadFile(hU, buf, sizeof(buf), &br) && br > 0) {
        if (g_CancelDownload) { of.close(); InternetCloseHandle(hU); InternetCloseHandle(hI); fs::remove(destPath, ec); return false; }
        of.write(buf, br); tr += br;
        DWORD now = GetTickCount(); if (now - lastTick > 50) { lastTick = now; SendProgress(total > 0 ? tr / total * 100.0 : 0.0, tr / 1048576.0, total / 1048576.0, statusKey); }
    }
    of.close(); InternetCloseHandle(hU); InternetCloseHandle(hI);
    if (total > 0 && (double)tr != total) { SendError("Download incomplete: " + std::to_string(tr) + "/" + std::to_string((long long)total)); fs::remove(destPath, ec); return false; }
    if (!expectSha.empty() && Sha256FileHex(destPath) != expectSha) { SendError("Integrity check failed"); fs::remove(destPath, ec); return false; }
    SendProgress(100, tr / 1048576.0, tr / 1048576.0, statusKey);
    return true;
}

bool UnzipWithPowerShell(const std::wstring& zipPath, const std::wstring& targetDir) {
    if (g_CancelDownload) return false;
    std::error_code ec; fs::create_directories(targetDir, ec);
    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"Expand-Archive -LiteralPath '" + zipPath + L"' -DestinationPath '" + targetDir + L"' -Force\"";
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi; std::vector<wchar_t> cb(cmd.begin(), cmd.end()); cb.push_back(0);
    if (CreateProcessW(NULL, cb.data(), NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE); DWORD e = 0; GetExitCodeProcess(pi.hProcess, &e);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        if (e != 0) { SendError("Unzip failed, code: " + std::to_string(e)); return false; }
        return true;
    }
    SendError("Failed to start PowerShell"); return false;
}

// =====================================================================================
//  Манифест с сервера (подписан, привязан к nonce - старый ответ подсунуть нельзя)
// =====================================================================================
struct FileRef { std::string url, sha256, name; long long size = 0; };
struct Manifest { std::string tier, mc, modVersion; FileRef jre, mod, fabric, game, assets; };

static FileRef ParseFile(const json& j, const char* k) {
    FileRef f; if (!j.contains(k) || !j[k].is_object()) return f; const json& o = j[k];
    f.url = o.value("url", std::string()); f.sha256 = o.value("sha256", std::string()); f.name = o.value("name", std::string()); f.size = o.value("size", 0LL); return f;
}

// key пустой -> default; иначе проверка ключа на сервере и бета-доступ
bool FetchManifest(const std::string& key, Manifest& m, std::string& err) {
    std::string host = ApiHost();
    if (host.find("example") != std::string::npos || PubX().find_first_not_of('0') == std::string::npos) { err = "config"; return false; } // API_HOST / PUB_X не заполнены
    std::string nonce = RandomHex(16);
    json req = { {"hwid",GetHwid()},{"nonce",nonce},{"v",LAUNCHER_VER} }; if (!key.empty()) req["key"] = key;
    std::string resp; DWORD st = 0;
    if (!HttpPostJson(host, L"/v1/manifest", req.dump(), resp, st)) { err = "network"; return false; }
    json j = json::parse(resp, nullptr, false);
    if (!j.is_object()) { err = "network"; return false; }
    if (!j.value("ok", false)) { err = j.value("msg", std::string("invalid_key")); return false; }
    std::string man = j.value("manifest", std::string()), sig = j.value("sig", std::string());
    if (!VerifyManifestSignature(man, sig)) { err = "signature"; return false; }
    json mj = json::parse(man, nullptr, false);
    if (!mj.is_object() || mj.value("nonce", std::string()) != nonce) { err = "signature"; return false; }
    m.tier = mj.value("tier", std::string("default")); m.mc = mj.value("mc", std::string());
    m.jre = ParseFile(mj, "jre"); m.mod = ParseFile(mj, "mod"); m.fabric = ParseFile(mj, "fabric_api"); m.game = ParseFile(mj, "game"); m.assets = ParseFile(mj, "assets");
    m.modVersion = mj.contains("mod") ? mj["mod"].value("version", std::string()) : std::string();
    if (m.mc != WideToUtf8(MC_VER) || m.mod.url.empty() || m.modVersion.empty()) { err = "signature"; return false; }
    return true;
}

// чистим старые версии мода/Fabric API, чтобы в игре не оказалось двух копий
void CleanupMods(const std::wstring& modsDir, const std::wstring& keepFabric) {
    std::error_code ec; if (!fs::exists(modsDir)) return;
    for (auto& e : fs::directory_iterator(modsDir, ec)) {
        if (!e.is_regular_file()) continue;
        std::wstring f = e.path().filename().wstring(), lo = f; std::transform(lo.begin(), lo.end(), lo.begin(), ::towlower);
        bool jar = lo.size() > 4 && lo.substr(lo.size() - 4) == L".jar";
        bool oldMod = jar && (lo.rfind(L"dune", 0) == 0 || lo.rfind(L"codex", 0) == 0) && f != MOD_FILENAME;
        bool oldApi = jar && lo.rfind(L"fabric-api", 0) == 0 && f != keepFabric;
        if (oldMod || oldApi) fs::remove(e.path(), ec);
    }
}

// =====================================================================================
//  Запуск игры (логика из старого лоадера без изменений, версия теперь Fabric 1.21.11)
// =====================================================================================
std::wstring FindJavaExe(bool useJavaw) {
    std::wstring base = GetBaseDir();
    std::wstring exeName = useJavaw ? L"javaw.exe" : L"java.exe";
    std::wstring je = base + L"jre\\bin\\" + exeName;
    if (fs::exists(je)) return je;
    if (fs::exists(base + L"jre")) {
        for (auto const& d : fs::directory_iterator(base + L"jre")) {
            if (d.is_directory()) {
                std::wstring sp = d.path().wstring() + L"\\bin\\" + exeName;
                if (fs::exists(sp)) return sp;
            }
        }
    }
    je = base + L"jre\\bin\\" + (useJavaw ? L"java.exe" : L"javaw.exe");
    if (fs::exists(je)) return je;
    if (fs::exists(base + L"jre")) {
        for (auto const& d : fs::directory_iterator(base + L"jre")) {
            if (d.is_directory()) {
                std::wstring sp = d.path().wstring() + L"\\bin\\" + (useJavaw ? L"java.exe" : L"javaw.exe");
                if (fs::exists(sp)) return sp;
            }
        }
    }
    return L"";
}

std::string ReadFileToString(const std::wstring& path) {
    std::ifstream f(path); if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string ExtractJsonString(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey); if (pos == std::string::npos) return "";
    size_t colon = json.find(':', pos + searchKey.length()); if (colon == std::string::npos) return "";
    size_t qs = json.find('"', colon + 1); if (qs == std::string::npos) return "";
    size_t qe = json.find('"', qs + 1); if (qe == std::string::npos) return "";
    return json.substr(qs + 1, qe - qs - 1);
}

std::string BuildClasspath() {
    std::wstring vd = GetVersionDir();
    std::wstring vj = vd + VERSION_NAME + L".jar";
    std::wstring libDir = GetMinecraftDir() + L"libraries";
    std::string cp = WideToUtf8(vj);

    auto parseVersion = [](const std::string& ver) -> std::vector<int> {
        std::vector<int> nums; std::string current;
        for (size_t i = 0; i < ver.size(); i++) {
            char c = ver[i];
            if (c == '.' || c == '-' || c == '_' || c == '+') {
                if (!current.empty()) { try { nums.push_back(std::stoi(current)); } catch (...) { nums.push_back(0); } current.clear(); }
            }
            else if (c >= '0' && c <= '9') { current += c; }
            else { if (!current.empty()) { try { nums.push_back(std::stoi(current)); } catch (...) { nums.push_back(0); } current.clear(); } }
        }
        if (!current.empty()) { try { nums.push_back(std::stoi(current)); } catch (...) { nums.push_back(0); } }
        while (nums.size() < 4) nums.push_back(0); return nums;
        };

    auto isVersionGreater = [&parseVersion](const std::string& a, const std::string& b) -> bool {
        auto va = parseVersion(a), vb = parseVersion(b);
        size_t len = va.size() < vb.size() ? va.size() : vb.size();
        for (size_t i = 0; i < len; i++) { if (va[i] > vb[i]) return true; if (va[i] < vb[i]) return false; }
        return false;
        };

    struct LibEntry { std::string key; std::string version; std::wstring fullPath; };
    std::map<std::string, LibEntry> bestLibs;

    if (fs::exists(libDir)) {
        for (auto const& entry : fs::recursive_directory_iterator(libDir)) {
            if (!entry.is_regular_file()) continue;
            std::wstring ext = entry.path().extension().wstring();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
            if (ext != L".jar") continue;
            std::wstring fullPath = entry.path().wstring();
            std::string fileName = WideToUtf8(entry.path().filename().wstring());
            if (fileName.find("natives-linux") != std::string::npos) continue;
            if (fileName.find("natives-macos") != std::string::npos) continue;
            if (fileName.find("natives-osx") != std::string::npos) continue;
            if (fileName.find("linux-aarch") != std::string::npos) continue;
            if (fileName.find("linux-x86_64") != std::string::npos) continue;
            std::wstring relPath = fullPath.substr(libDir.length());
            if (!relPath.empty() && (relPath[0] == L'\\' || relPath[0] == L'/')) relPath = relPath.substr(1);
            std::string relUtf8 = WideToUtf8(relPath);
            std::replace(relUtf8.begin(), relUtf8.end(), '\\', '/');
            std::vector<std::string> pathParts; std::istringstream ss(relUtf8); std::string tok;
            while (std::getline(ss, tok, '/')) { if (!tok.empty()) pathParts.push_back(tok); }
            std::string key, version;
            if (pathParts.size() >= 4) {
                version = pathParts[pathParts.size() - 2];
                std::string artifact = pathParts[pathParts.size() - 3]; std::string group;
                for (size_t i = 0; i < pathParts.size() - 3; i++) { if (!group.empty()) group += "."; group += pathParts[i]; }
                key = group + ":" + artifact;
                if (fileName.find("natives-windows") != std::string::npos) {
                    key += ":natives-windows";
                    if (fileName.find("arm64") != std::string::npos) key += "-arm64";
                    else if (fileName.find("x86") != std::string::npos && fileName.find("x86_64") == std::string::npos) key += "-x86";
                }
            }
            else { key = WideToUtf8(fullPath); version = "0"; }
            auto it = bestLibs.find(key);
            if (it == bestLibs.end()) { LibEntry le; le.key = key; le.version = version; le.fullPath = fullPath; bestLibs[key] = le; }
            else { if (isVersionGreater(version, it->second.version)) { it->second.version = version; it->second.fullPath = fullPath; } }
        }
    }
    for (auto it = bestLibs.begin(); it != bestLibs.end(); ++it) { cp += ";" + WideToUtf8(it->second.fullPath); }
    return cp;
}

std::string GetMainClass() {
    std::wstring jp = GetVersionDir() + VERSION_NAME + L".json";
    if (fs::exists(jp)) { std::string jc = ReadFileToString(jp); std::string mc = ExtractJsonString(jc, "mainClass"); if (!mc.empty()) return mc; }
    return "net.fabricmc.loader.impl.launch.knot.KnotClient";
}

std::string GetAssetIndex() {
    std::wstring jp = GetVersionDir() + VERSION_NAME + L".json";
    if (fs::exists(jp)) {
        std::string jc = ReadFileToString(jp);
        size_t aiPos = jc.find("\"assetIndex\"");
        if (aiPos != std::string::npos) {
            size_t braceStart = jc.find('{', aiPos);
            if (braceStart != std::string::npos) {
                size_t braceEnd = jc.find('}', braceStart);
                if (braceEnd != std::string::npos) {
                    std::string aiBlock = jc.substr(braceStart, braceEnd - braceStart + 1);
                    std::string id = ExtractJsonString(aiBlock, "id"); if (!id.empty()) return id;
                }
            }
        }
        std::string assets = ExtractJsonString(jc, "assets"); if (!assets.empty()) return assets;
    }
    return "21";
}

void MonitorProcessThread(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid); if (h) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
    g_GamePID = 0; SendType("process_stopped");
}

void LogCommandLine(const std::wstring& cmd) {
    std::wstring logPath = GetBaseDir() + L"launch_cmd.log";
    std::ofstream f(logPath, std::ios::trunc); if (f) { f << WideToUtf8(cmd); f.close(); }
}

int GetSafeRamAmount(int requested) {
    MEMORYSTATUSEX memInfo; memInfo.dwLength = sizeof(MEMORYSTATUSEX);
    if (GlobalMemoryStatusEx(&memInfo)) {
        int totalMb = (int)(memInfo.ullTotalPhys / 1024 / 1024);
        int maxAllowed = totalMb - 1024; if (maxAllowed < 1024) maxAllowed = 1024;
        maxAllowed = (maxAllowed / 128) * 128;
        if (requested > maxAllowed) {
            SendError(L"RAM " + std::to_wstring(requested) + L"MB exceeds available (" + std::to_wstring(totalMb) + L"MB total), clamped to " + std::to_wstring(maxAllowed) + L"MB");
            return maxAllowed;
        }
    }
    return requested;
}

std::wstring GetSafeNickname() {
    std::wstring nick = g_P.nick; std::wstring safe;
    for (wchar_t c : nick) {
        if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_') safe += c;
    }
    if (safe.empty()) safe = L"Player"; if (safe.length() > 16) safe = safe.substr(0, 16);
    return safe;
}

void LaunchGame() {
    std::wstring javawExe = FindJavaExe(true);
    if (javawExe.empty()) javawExe = FindJavaExe(false);
    if (javawExe.empty()) { SendError(L"No Java executable found"); return; }

    int safeRam = GetSafeRamAmount(g_P.ram);
    std::string cpStr = BuildClasspath();
    std::string mc = GetMainClass();
    std::string assetIndex = GetAssetIndex();
    std::wstring md = GetMinecraftDir();
    std::wstring nd = GetVersionDir() + L"natives";
    if (!fs::exists(nd)) fs::create_directories(nd);
    if (!md.empty() && md.back() == L'\\') md.pop_back();
    if (!nd.empty() && nd.back() == L'\\') nd.pop_back();
    std::wstring assetsDir = GetMinecraftDir() + L"assets";
    if (!assetsDir.empty() && assetsDir.back() == L'\\') assetsDir.pop_back();

    std::wstring safeNick = GetSafeNickname();

    std::vector<std::wstring> jvmArgs;
    jvmArgs.push_back(L"-Xmx" + std::to_wstring(safeRam) + L"M");
    jvmArgs.push_back(L"-Xms512M"); jvmArgs.push_back(L"-Xss1M");
    jvmArgs.push_back(L"-XX:+UseG1GC"); jvmArgs.push_back(L"-XX:+UnlockExperimentalVMOptions");
    jvmArgs.push_back(L"-XX:G1NewSizePercent=20"); jvmArgs.push_back(L"-XX:G1ReservePercent=20");
    jvmArgs.push_back(L"-XX:MaxGCPauseMillis=50"); jvmArgs.push_back(L"-XX:G1HeapRegionSize=32M");
    jvmArgs.push_back(L"-XX:HeapDumpPath=MojangTricksIntelDriversForPerformance_javaw.exe_minecraft.exe.heapdump");
    jvmArgs.push_back(L"-Djava.library.path=" + nd);
    jvmArgs.push_back(L"-Djna.tmpdir=" + nd);
    jvmArgs.push_back(L"-Dorg.lwjgl.system.SharedLibraryExtractPath=" + nd);
    jvmArgs.push_back(L"-Dio.netty.native.workdir=" + nd);
    jvmArgs.push_back(L"-Dminecraft.launcher.brand=custom-launcher");
    jvmArgs.push_back(L"-Dminecraft.launcher.version=2.0");
    jvmArgs.push_back(L"-DFabricMcEmu= net.minecraft.client.main.Main ");
    jvmArgs.push_back(L"-cp"); jvmArgs.push_back(Utf8ToWide(cpStr));

    std::vector<std::wstring> gameArgs;
    gameArgs.push_back(L"--username"); gameArgs.push_back(safeNick);
    gameArgs.push_back(L"--version"); gameArgs.push_back(VERSION_NAME);
    gameArgs.push_back(L"--gameDir"); gameArgs.push_back(md);
    gameArgs.push_back(L"--assetsDir"); gameArgs.push_back(assetsDir);
    gameArgs.push_back(L"--assetIndex"); gameArgs.push_back(Utf8ToWide(assetIndex));
    gameArgs.push_back(L"--uuid"); gameArgs.push_back(L"00000000-0000-0000-0000-000000000000");
    gameArgs.push_back(L"--accessToken"); gameArgs.push_back(L"0");
    gameArgs.push_back(L"--userType"); gameArgs.push_back(L"legacy");
    gameArgs.push_back(L"--versionType"); gameArgs.push_back(L"release");

    auto quoteIfNeeded = [](const std::wstring& s) -> std::wstring {
        if (s.find(L' ') != std::wstring::npos || s.find(L'\t') != std::wstring::npos) return L"\"" + s + L"\"";
        return s;
        };

    std::wstring cmd = quoteIfNeeded(javawExe);
    for (auto& a : jvmArgs) cmd += L" " + quoteIfNeeded(a);
    cmd += L" " + Utf8ToWide(mc);
    for (auto& a : gameArgs) cmd += L" " + quoteIfNeeded(a);
    LogCommandLine(cmd);

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end()); cmdBuf.push_back(0);
    std::wstring elp = GetBaseDir() + L"launch_error.log";
    SECURITY_ATTRIBUTES sa; sa.nLength = sizeof(sa); sa.lpSecurityDescriptor = NULL; sa.bInheritHandle = TRUE;
    HANDLE hLog = CreateFileW(elp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si = { sizeof(si) };
    if (hLog != INVALID_HANDLE_VALUE) { si.dwFlags |= STARTF_USESTDHANDLES; si.hStdError = hLog; si.hStdOutput = hLog; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE); }
    PROCESS_INFORMATION pi;
    if (CreateProcessW(NULL, cmdBuf.data(), NULL, NULL, TRUE, 0, NULL, md.c_str(), &si, &pi)) {
        g_GamePID = pi.dwProcessId;
        HANDLE hPC = pi.hProcess, hLC = hLog; std::wstring elpCopy = elp;
        std::thread([hPC, hLC, elpCopy]() {
            WaitForSingleObject(hPC, 30000); DWORD exitCode = 0; GetExitCodeProcess(hPC, &exitCode);
            if (exitCode != STILL_ACTIVE && exitCode != 0) {
                CloseHandle(hPC); if (hLC != INVALID_HANDLE_VALUE) CloseHandle(hLC);
                std::wstring errMsg = L"Java exited code " + std::to_wstring(exitCode);
                if (fs::exists(elpCopy) && fs::file_size(elpCopy) > 0) {
                    std::string content = ReadFileToString(elpCopy);
                    std::istringstream iss(content); std::vector<std::string> lines; std::string line;
                    while (std::getline(iss, line)) lines.push_back(line);
                    int start = (int)lines.size() > 10 ? (int)lines.size() - 10 : 0; std::string last;
                    for (int i = start; i < (int)lines.size(); i++) {
                        std::string c = lines[i]; size_t p;
                        while ((p = c.find('"')) != std::string::npos) c.replace(p, 1, "'");
                        while ((p = c.find('\\')) != std::string::npos) c.replace(p, 1, "/");
                        last += c + " | ";
                    }
                    if (!last.empty()) errMsg += L" :: " + Utf8ToWide(last);
                }
                SendError(errMsg);
            }
            else { CloseHandle(hPC); if (hLC != INVALID_HANDLE_VALUE) CloseHandle(hLC); }
            }).detach();
        std::thread(MonitorProcessThread, pi.dwProcessId).detach();
        CloseHandle(pi.hThread);
    }
    else { DWORD e = GetLastError(); if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog); SendError(L"CreateProcess failed: " + std::to_wstring(e)); }
}

void TerminateGame() {
    DWORD pid = g_GamePID; if (pid != 0) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid); if (h) { TerminateProcess(h, 0); CloseHandle(h); } g_GamePID = 0;
    }
}

// =====================================================================================
//  Установка / обновление / запуск
// =====================================================================================
struct BusyGuard { ~BusyGuard() { g_Busy = false; json j; j["type"] = "busy"; j["value"] = false; SafePostJson(j); } };

static std::wstring SafeName(const std::string& n) { return fs::path(Utf8ToWide(n)).filename().wstring(); }
static std::wstring ModTag(const Manifest& m) { return Utf8ToWide(m.tier + ":" + m.modVersion); }

bool InstallAndLaunch(const Manifest& m) {
    g_CancelDownload = false;
    std::wstring base = GetBaseDir(), md = GetMinecraftDir(), modd = GetModsDir(); std::error_code ec;
    fs::create_directories(base, ec); fs::create_directories(md, ec); fs::create_directories(modd, ec);
    fs::remove_all(md + L"versions\\Fabric 1.21.4", ec); // старая версия больше не нужна
    std::wstring apiName = SafeName(m.fabric.name);
    bool ok = true;
    if (ok) ok = DownloadFile(m.jre.url, base + L"jre.zip", "dl_java", m.jre.sha256, m.jre.size);
    if (ok) { SendProgress(100, 0, 0, "unz_java"); fs::remove_all(base + L"jre", ec); ok = UnzipWithPowerShell(base + L"jre.zip", base + L"jre"); fs::remove(base + L"jre.zip", ec); }
    CleanupMods(modd, apiName);
    if (ok) ok = DownloadFile(m.mod.url, modd + MOD_FILENAME, "dl_mod", m.mod.sha256, m.mod.size);
    if (ok) ok = DownloadFile(m.fabric.url, modd + apiName, "dl_fabric", m.fabric.sha256, m.fabric.size);
    if (ok) ok = DownloadFile(m.game.url, base + L"game.zip", "dl_game", m.game.sha256, m.game.size);
    if (ok) { SendProgress(100, 0, 0, "unz_game"); ok = UnzipWithPowerShell(base + L"game.zip", md); fs::remove(base + L"game.zip", ec); }
    if (ok) ok = DownloadFile(m.assets.url, base + L"assets.zip", "dl_assets", m.assets.sha256, m.assets.size);
    if (ok) { SendProgress(100, 0, 0, "unz_assets"); ok = UnzipWithPowerShell(base + L"assets.zip", md); fs::remove(base + L"assets.zip", ec); }
    if (g_CancelDownload) return false;
    if (ok) { std::wstring je = FindJavaExe(true); if (je.empty()) je = FindJavaExe(false); if (je.empty()) { SendError("Java not found after install"); ok = false; } }
    if (ok) {
        if (!fs::exists(GetVersionDir() + VERSION_NAME + L".json")) { SendError(L"Version JSON missing: " + GetVersionDir() + VERSION_NAME + L".json"); ok = false; }
        if (!fs::exists(GetVersionDir() + VERSION_NAME + L".jar")) { SendError(L"Version JAR missing: " + GetVersionDir() + VERSION_NAME + L".jar"); ok = false; }
    }
    if (ok) {
        { std::lock_guard<std::mutex> lk(g_Pm); g_P.installed = true; g_P.mcVer = MC_VER; g_P.modTag = ModTag(m); } SavePrefs();
        SendType("finish_install"); std::this_thread::sleep_for(std::chrono::milliseconds(2500)); LaunchGame();
    }
    else {
        SendProgress(0, 0, 0, "error");
        { std::lock_guard<std::mutex> lk(g_Pm); g_P.installed = false; } SavePrefs();
    }
    return ok;
}

// лёгкий апдейт: только мод (+ Fabric API, если его нет) - без JRE/Minecraft/assets
bool UpdateModAndLaunch(const Manifest& m) {
    g_CancelDownload = false;
    std::wstring modd = GetModsDir(); std::error_code ec; fs::create_directories(modd, ec);
    std::wstring apiName = SafeName(m.fabric.name);
    CleanupMods(modd, apiName);
    bool ok = DownloadFile(m.mod.url, modd + MOD_FILENAME, "upd_mod", m.mod.sha256, m.mod.size);
    if (ok && !fs::exists(modd + apiName)) ok = DownloadFile(m.fabric.url, modd + apiName, "dl_fabric", m.fabric.sha256, m.fabric.size);
    if (g_CancelDownload) return false;
    if (ok) {
        { std::lock_guard<std::mutex> lk(g_Pm); g_P.modTag = ModTag(m); } SavePrefs();
        SendType("finish_install"); std::this_thread::sleep_for(std::chrono::milliseconds(500)); LaunchGame();
    }
    else SendProgress(0, 0, 0, "error");
    return ok;
}

void SendLogin(bool ok, const std::string& tier, const std::string& msg, bool skip = false) {
    json j; j["type"] = "login_result"; j["ok"] = ok; j["tier"] = tier; j["msg"] = msg; j["skip"] = skip; SafePostJson(j);
}

void LoginThread(std::string key) {
    Manifest m; std::string err;
    if (FetchManifest(key, m, err)) {
        SetTier(m.tier);
        { std::lock_guard<std::mutex> lk(g_Pm); g_P.key = (m.tier == "beta") ? key : ""; g_P.skipped = (m.tier != "beta"); } SavePrefs();
        SendLogin(true, m.tier, "");
    }
    else {
        if (err != "network" && err != "config") { std::lock_guard<std::mutex> lk(g_Pm); g_P.key.clear(); }
        SavePrefs(); SetTier("none"); SendLogin(false, "none", err);
    }
}

void StartProcessLogic() {
    if (g_GamePID != 0) { TerminateGame(); return; }
    if (g_Busy.exchange(true)) return;
    { json j; j["type"] = "busy"; j["value"] = true; SafePostJson(j); }
    std::thread([]() {
        BusyGuard guard;
        Prefs p; { std::lock_guard<std::mutex> lk(g_Pm); p = g_P; }
        std::string tier = GetTier();
        Manifest m; std::string err;
        std::wstring versionJar = GetVersionDir() + VERSION_NAME + L".jar", modPath = GetModsDir() + MOD_FILENAME;
        bool coreOk = p.installed && p.mcVer == MC_VER && fs::exists(versionJar) && fs::exists(GetBaseDir() + L"jre");

        if (!FetchManifest(tier == "beta" ? p.key : "", m, err)) {
            bool keyProblem = (err == "invalid_key" || err == "revoked" || err == "expired" || err == "bound_other_pc");
            if (keyProblem) { SetTier("none"); { std::lock_guard<std::mutex> lk(g_Pm); g_P.key.clear(); } SavePrefs(); SendLogin(false, "none", err); }
            else if (tier != "beta" && coreOk && fs::exists(modPath) && err == "network") { SendType("launch_success"); LaunchGame(); } // default: можно из кеша без сети
            else SendError(err);
            return;
        }
        SetTier(m.tier);
        std::wstring apiName = SafeName(m.fabric.name);
        bool modOk = coreOk && p.modTag == ModTag(m) && fs::exists(modPath) && fs::exists(GetModsDir() + apiName) &&
            (m.mod.sha256.empty() || Sha256FileHex(modPath) == m.mod.sha256); // целостность jar проверяется при каждом запуске
        if (coreOk && modOk) { SendType("launch_success"); LaunchGame(); }
        else if (coreOk) { SendType("start_load"); UpdateModAndLaunch(m); }
        else { SendType("start_load"); InstallAndLaunch(m); }
        }).detach();
}

// =====================================================================================
//  Окно + WebView2 (UI отдаётся из ресурсов exe по виртуальному адресу https://dune.local/)
// =====================================================================================
static const wchar_t* UI_ORIGIN = L"https://dune.local/";

static std::pair<const BYTE*, DWORD> GetRes(const wchar_t* name) {
    HRSRC r = FindResourceW(nullptr, name, RT_RCDATA); if (!r) return { nullptr, 0 };
    HGLOBAL g = LoadResource(nullptr, r); return { (const BYTE*)LockResource(g), SizeofResource(nullptr, r) };
}

void HandleUiMessage(const std::wstring& msg) {
    if (msg == L"close") DestroyWindow(g_hWnd);
    else if (msg == L"minimize") ShowWindow(g_hWnd, SW_MINIMIZE);
    else if (msg == L"drag_window") { ReleaseCapture(); SendMessage(g_hWnd, WM_NCLBUTTONDOWN, HTCAPTION, 0); }
    else if (msg == L"action_button") StartProcessLogic();
    else if (msg == L"cancel_install") g_CancelDownload = true;
    else if (msg == L"open_site") ShellExecuteW(nullptr, L"open", SITE_URL, nullptr, nullptr, SW_SHOWNORMAL);
    else if (msg == L"open_mods") { std::error_code ec; fs::create_directories(GetModsDir(), ec); ShellExecuteW(nullptr, L"open", GetModsDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    else if (msg == L"skip") {
        SetTier("default"); { std::lock_guard<std::mutex> lk(g_Pm); g_P.key.clear(); g_P.skipped = true; } SavePrefs(); SendLogin(true, "default", "", true);
    }
    else if (msg == L"logout") { SetTier("none"); { std::lock_guard<std::mutex> lk(g_Pm); g_P.key.clear(); g_P.skipped = false; } SavePrefs(); }
    else if (msg.rfind(L"login:", 0) == 0) { std::string k = CleanKey(WideToUtf8(msg.substr(6))); std::thread(LoginThread, k).detach(); }
    else if (msg.rfind(L"save_nick:", 0) == 0) {
        std::wstring safe; for (wchar_t c : msg.substr(10)) if ((c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z') || (c >= L'0' && c <= L'9') || c == L'_') safe += c;
        if (safe.empty()) safe = L"Player"; if (safe.length() > 16) safe = safe.substr(0, 16);
        { std::lock_guard<std::mutex> lk(g_Pm); g_P.nick = safe; } SavePrefs();
        json j; j["type"] = "set_nick"; j["value"] = WideToUtf8(safe); SafePostJson(j);
    }
    else if (msg.rfind(L"save_ram:", 0) == 0) {
        int req = 4028; try { req = std::stoi(msg.substr(9)); } catch (...) {}
        int safe = GetSafeRamAmount(req); { std::lock_guard<std::mutex> lk(g_Pm); g_P.ram = safe; } SavePrefs();
        if (safe != req) { json j; j["type"] = "set_ram"; j["value"] = safe; SafePostJson(j); }
    }
    else if (msg.rfind(L"set_theme:", 0) == 0) { { std::lock_guard<std::mutex> lk(g_Pm); g_P.dark = (msg.substr(10) == L"dark"); g_P.hasPrefs = true; } SavePrefs(); }
    else if (msg.rfind(L"set_lang:", 0) == 0) { { std::lock_guard<std::mutex> lk(g_Pm); g_P.ru = (msg.substr(9) == L"ru"); g_P.hasPrefs = true; } SavePrefs(); }
}

void SendInit() {
    Prefs p; { std::lock_guard<std::mutex> lk(g_Pm); p = g_P; }
    std::string state = "key";
    if (!p.key.empty()) state = "autologin";
    else if (p.skipped) { state = "main"; SetTier("default"); }
    json j; j["type"] = "init"; j["lang"] = p.ru ? "ru" : "en"; j["theme"] = p.dark ? "dark" : "light"; j["nick"] = WideToUtf8(p.nick);
    j["ram"] = p.ram; j["state"] = state; j["tier"] = GetTier(); SafePostJson(j);
    if (state == "autologin") std::thread(LoginThread, p.key).detach();
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
    case WM_WEBVIEW_UPDATE: { std::wstring* j = (std::wstring*)lParam; if (g_webview) g_webview->PostWebMessageAsJson(j->c_str()); delete j; return 0; }
    case WM_SIZE: if (g_webviewController) { RECT b; GetClientRect(hWnd, &b); g_webviewController->put_Bounds(b); } break;
    case WM_DESTROY: TerminateGame(); PostQuitMessage(0); break;
    default: return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
#ifndef _DEBUG
    // базовая защита от отладчика (релизная сборка)
    BOOL remote = FALSE; CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    if (IsDebuggerPresent() || remote) return 0;
#endif
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    LoadPrefs();
    std::error_code ec0; fs::create_directories(GetBaseDir(), ec0);

    WNDCLASSEXW wcex = { sizeof(WNDCLASSEX) }; wcex.style = CS_HREDRAW | CS_VREDRAW; wcex.lpfnWndProc = WndProc;
    wcex.hInstance = hInstance; wcex.hCursor = LoadCursor(nullptr, IDC_ARROW); wcex.lpszClassName = L"LauncherClass";
    wcex.hIcon = LoadIconW(hInstance, L"IDI_ICON1"); RegisterClassExW(&wcex);
    int sW = GetSystemMetrics(SM_CXSCREEN), sH = GetSystemMetrics(SM_CYSCREEN);
    g_hWnd = CreateWindowExW(WS_EX_LAYERED, L"LauncherClass", CHEAT_NAME.c_str(), WS_POPUP | WS_VISIBLE, (sW - WIN_W) / 2, (sH - WIN_H) / 2, WIN_W, WIN_H, nullptr, nullptr, hInstance, nullptr);
    DWM_WINDOW_CORNER_PREFERENCE pref = DWMWCP_ROUND; DwmSetWindowAttribute(g_hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    SetLayeredWindowAttributes(g_hWnd, 0, 255, LWA_ALPHA);

    CreateCoreWebView2EnvironmentWithOptions(nullptr, (GetBaseDir() + L"cache").c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) return S_OK; g_env = env;
                env->CreateCoreWebView2Controller(g_hWnd, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [](HRESULT, ICoreWebView2Controller* controller) -> HRESULT {
                        if (!controller) return S_OK;
                        g_webviewController = controller; g_webviewController->get_CoreWebView2(&g_webview);
                        if (auto c2 = g_webviewController.try_query<ICoreWebView2Controller2>()) { COREWEBVIEW2_COLOR col = { 255, 20, 23, 28 }; c2->put_DefaultBackgroundColor(col); }
                        wil::com_ptr<ICoreWebView2Settings> st; g_webview->get_Settings(&st);
                        st->put_AreDefaultContextMenusEnabled(FALSE); st->put_AreDevToolsEnabled(FALSE); st->put_IsStatusBarEnabled(FALSE); st->put_IsZoomControlEnabled(FALSE);
                        if (auto s3 = st.try_query<ICoreWebView2Settings3>()) s3->put_AreBrowserAcceleratorKeysEnabled(FALSE); // F12, Ctrl+U, Ctrl+S и т.п.
                        RECT b; GetClientRect(g_hWnd, &b); g_webviewController->put_Bounds(b);

                        // UI и картинка отдаются прямо из ресурсов exe (на диске html-файлов нет)
                        g_webview->AddWebResourceRequestedFilter(L"https://dune.local/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
                        g_webview->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) -> HRESULT {
                                wil::com_ptr<ICoreWebView2WebResourceRequest> rq; args->get_Request(&rq);
                                wil::unique_cotaskmem_string uri; rq->get_Uri(&uri);
                                std::wstring path = std::wstring(uri.get()).substr(wcslen(UI_ORIGIN) - 1); size_t q = path.find_first_of(L"?#"); if (q != std::wstring::npos) path.resize(q);
                                const wchar_t* res = nullptr; std::wstring mime;
                                if (path == L"/" || path == L"/index.html") { res = L"UI_HTML"; mime = L"text/html; charset=utf-8"; }
                                else if (path == L"/bg.jpg") { res = L"BG_JPG"; mime = L"image/jpeg"; }
                                wil::com_ptr<ICoreWebView2WebResourceResponse> resp;
                                if (res) {
                                    auto r = GetRes(res); IStream* s = SHCreateMemStream(r.first, r.second);
                                    g_env->CreateWebResourceResponse(s, 200, L"OK", (L"Content-Type: " + mime + L"\r\nCache-Control: no-store").c_str(), &resp);
                                    if (s) s->Release();
                                }
                                else g_env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
                                args->put_Response(resp.get()); return S_OK;
                            }).Get(), nullptr);

                        // никаких переходов и новых окон, кроме нашего UI
                        g_webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* a) -> HRESULT {
                                wil::unique_cotaskmem_string u; a->get_Uri(&u); if (wcsncmp(u.get(), UI_ORIGIN, wcslen(UI_ORIGIN)) != 0) a->put_Cancel(TRUE); return S_OK; }).Get(), nullptr);
                        g_webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* a) -> HRESULT { a->put_Handled(TRUE); return S_OK; }).Get(), nullptr);

                        g_webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                wil::unique_cotaskmem_string src; args->get_Source(&src);
                                if (!src || wcsncmp(src.get(), UI_ORIGIN, wcslen(UI_ORIGIN)) != 0) return S_OK; // сообщения только от нашей страницы
                                wil::unique_cotaskmem_string pw; if (FAILED(args->TryGetWebMessageAsString(&pw)) || !pw) return S_OK;
                                HandleUiMessage(pw.get()); return S_OK;
                            }).Get(), nullptr);
                        g_webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                            [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT { SendInit(); return S_OK; }).Get(), nullptr);
                        g_webview->Navigate(L"https://dune.local/index.html");
                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());

    MSG msg; while (GetMessage(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}
