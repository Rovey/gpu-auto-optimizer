#include "hw/update_io.hpp"
#include "hw/app_files.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>

#include <cstdio>
#include <functional>
#include <iterator>
#include <vector>

namespace gao {

namespace {

struct HttpHandle {
    HINTERNET h = nullptr;
    ~HttpHandle() { if (h) WinHttpCloseHandle(h); }
    explicit operator bool() const { return h != nullptr; }
};

bool fail(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

// One GET; every chunk of the body goes to `sink`. False + *error on a
// refused URL, a transport error, a status other than 200, a body over
// max_bytes, or a sink that returns false.
bool https_fetch(const std::string& url, std::size_t max_bytes, const std::function<bool(const char*, std::size_t)>& sink,
                 std::string* error) {
    if (url.compare(0, 8, "https://") != 0) return fail(error, "not an https address");
    const std::wstring wide = widen(url);
    URL_COMPONENTS parts{sizeof(parts)};
    wchar_t host[256], path[2048];
    parts.lpszHostName = host;
    parts.dwHostNameLength = static_cast<DWORD>(std::size(host));
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = static_cast<DWORD>(std::size(path));
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        return fail(error, "the address could not be read");

    const HttpHandle session{WinHttpOpen(L"GpuAutoOptimizer", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                                         WINHTTP_NO_PROXY_BYPASS, 0)};
    if (!session) return fail(error, "no internet session (error " + std::to_string(GetLastError()) + ")");
    WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
    const HttpHandle connection{WinHttpConnect(session.h, host, parts.nPort, 0)};
    if (!connection) return fail(error, "could not connect (error " + std::to_string(GetLastError()) + ")");
    const HttpHandle request{WinHttpOpenRequest(connection.h, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                WINHTTP_FLAG_SECURE)};
    if (!request) return fail(error, "could not open the request (error " + std::to_string(GetLastError()) + ")");
    // The default already refuses a redirect from https to http; said here so
    // that a changed default cannot weaken it.
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
    if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr))
        return fail(error, "no answer (error " + std::to_string(GetLastError()) + ")");
    DWORD status = 0, size = sizeof(status);
    if (!WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status,
                             &size, WINHTTP_NO_HEADER_INDEX))
        return fail(error, "no status in the answer");
    if (status != 200) return fail(error, "the server answered " + std::to_string(status));

    std::vector<char> buffer(64 * 1024);
    std::size_t total = 0;
    for (;;) {
        DWORD got = 0;
        if (!WinHttpReadData(request.h, buffer.data(), static_cast<DWORD>(buffer.size()), &got))
            return fail(error, "the download broke off (error " + std::to_string(GetLastError()) + ")");
        if (got == 0) return true;
        total += got;
        if (total > max_bytes) return fail(error, "the download is larger than expected");
        if (!sink(buffer.data(), got)) return fail(error, "could not store the download");
    }
}

}

std::optional<std::string> https_get(const std::string& url, std::size_t max_bytes, std::string* error) {
    std::string body;
    if (!https_fetch(url, max_bytes, [&](const char* data, std::size_t n) { body.append(data, n); return true; }, error))
        return std::nullopt;
    return body;
}

bool https_download(const std::string& url, const std::filesystem::path& to, std::size_t max_bytes, std::string* error) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, to.c_str(), L"wb") != 0 || !f) return fail(error, "could not create " + to.string());
    bool ok = https_fetch(url, max_bytes, [&](const char* data, std::size_t n) { return std::fwrite(data, 1, n, f) == n; }, error);
    if (std::fclose(f) != 0 && ok) ok = fail(error, "could not write " + to.string());
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(to, ec);
    }
    return ok;
}

std::string sha256_hex(const std::filesystem::path& file) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, file.c_str(), L"rb") != 0 || !f) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32];
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
              BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<unsigned char> buffer(64 * 1024);
    while (ok) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), f);
        if (n == 0) break;
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), static_cast<ULONG>(n), 0));
    }
    ok = ok && !std::ferror(f) && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    std::fclose(f);
    if (!ok) return {};
    std::string hex;
    for (const unsigned char byte : digest) {
        constexpr char digits[] = "0123456789abcdef";
        hex += digits[byte >> 4];
        hex += digits[byte & 15];
    }
    return hex;
}

bool extract_zip(const std::filesystem::path& zip, const std::filesystem::path& into, std::string* error) {
    wchar_t system[MAX_PATH];
    const UINT n = GetSystemDirectoryW(system, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return fail(error, "the System32 folder could not be resolved");
    const std::filesystem::path tar = std::filesystem::path(system) / L"tar.exe";
    std::wstring command = L"\"" + tar.wstring() + L"\" -xf \"" + zip.wstring() + L"\" -C \"" + into.wstring() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(tar.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, system, &si, &pi))
        return fail(error, "could not start tar.exe (error " + std::to_string(GetLastError()) + ")");
    DWORD code = 1;
    const bool finished = WaitForSingleObject(pi.hProcess, 60000) == WAIT_OBJECT_0;
    if (!finished) TerminateProcess(pi.hProcess, 1);
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (!finished) return fail(error, "unpacking took too long");
    if (code != 0) return fail(error, "unpacking failed (tar.exe exit " + std::to_string(code) + ")");
    return true;
}

std::optional<unsigned> file_version_number(const std::filesystem::path& exe) {
    DWORD ignored = 0;
    const DWORD size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size == 0) return std::nullopt;
    std::vector<unsigned char> data(size);
    VS_FIXEDFILEINFO* info = nullptr;
    UINT length = 0;
    if (!GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) ||
        !VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &length) || !info || length < sizeof(*info))
        return std::nullopt;
    const unsigned major = HIWORD(info->dwFileVersionMS), minor = LOWORD(info->dwFileVersionMS), patch = HIWORD(info->dwFileVersionLS);
    if (major > 255 || minor > 255 || patch > 255) return std::nullopt;
    return (major << 16) | (minor << 8) | patch;
}

}
