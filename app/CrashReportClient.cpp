#include "app/CrashReportClient.h"

#include "engine/core/CrashReport.h"
#include "engine/core/StoredZip.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <CommCtrl.h>
#include <Shellapi.h>
#include <Winhttp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <nlohmann/json.hpp>

#if defined(_MSC_VER)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

namespace engine::app {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr std::size_t MaxArchiveBytes = 64U * 1024U * 1024U;
constexpr std::size_t MaxPostBytes = 4U * 1024U * 1024U;
constexpr std::size_t MaxMetadataBytes = 16U * 1024U;
constexpr DWORD MaxUrlLength = 2048;
constexpr DWORD MaxSignedUrlLength = 8192;
constexpr wchar_t EndpointVariable[] = L"TGE_CRASH_REPORT_URL";
constexpr wchar_t ReportIdHeader[] = L"X-TGE-Report-Id";
constexpr wchar_t UploadUrlHeader[] = L"X-TGE-Upload-Url";
constexpr wchar_t ZipContentType[] = L"Content-Type: application/zip\r\n";
constexpr wchar_t JsonContentType[] = L"Content-Type: application/json\r\n";
constexpr int SendButton = 100;
constexpr int OpenFolderButton = 101;

struct CloseFile {
    void operator()(HANDLE file) const noexcept { CloseHandle(file); }
};

struct CloseHttp {
    void operator()(HINTERNET handle) const noexcept { WinHttpCloseHandle(handle); }
};

using FileHandle = std::unique_ptr<void, CloseFile>;
using HttpHandle = std::unique_ptr<void, CloseHttp>;

struct Endpoint {
    std::wstring url;
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port{};
    bool secure{};
};

struct Bundle {
    std::string reportId;
    std::string zip;
};

struct HttpResponse {
    HttpHandle connection;
    HttpHandle request;
    DWORD status{};
};

struct SendResult {
    std::string reportId;
    std::string error;
};

std::wstring utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const auto length = static_cast<int>(value.size());
    const auto required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (!required) throw std::runtime_error("Invalid UTF-8 text.");
    std::wstring converted(static_cast<std::size_t>(required), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, converted.data(), required)) {
        throw std::runtime_error("Invalid UTF-8 text.");
    }
    return converted;
}

std::string readFile(const fs::path& path, const char* name, std::size_t maxBytes) {
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::runtime_error(std::string("Could not open ") + name + ".");
    const FileHandle file(handle);
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file.get()) != FILE_TYPE_DISK || !GetFileInformationByHandle(file.get(), &info) ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        throw std::runtime_error(std::string("Expected a regular file for ") + name + ".");
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size) || size.QuadPart <= 0 ||
        static_cast<std::uint64_t>(size.QuadPart) > maxBytes) {
        throw std::runtime_error(std::string("Empty or oversized file: ") + name + ".");
    }
    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read{};
    if (!ReadFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
        read != bytes.size()) {
        throw std::runtime_error(std::string("Could not read ") + name + ".");
    }
    return bytes;
}

fs::path executablePath() {
    std::array<wchar_t, 32768> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("Could not locate the application.");
    return fs::path(std::wstring_view(buffer.data(), length));
}

std::wstring readUrl(const fs::path& exe) {
    std::array<wchar_t, MaxUrlLength + 1> buffer{};
    SetLastError(ERROR_SUCCESS);
    const auto size = GetEnvironmentVariableW(EndpointVariable, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size >= buffer.size()) throw std::runtime_error("TGE_CRASH_REPORT_URL is too long.");
    if (size) return std::wstring(buffer.data(), size);
    if (GetLastError() != ERROR_ENVVAR_NOT_FOUND) throw std::runtime_error("TGE_CRASH_REPORT_URL is empty.");

    const auto config = exe.parent_path() / L"crash-report-url.txt";
    if (!fs::exists(config)) {
        throw std::runtime_error("Set TGE_CRASH_REPORT_URL or configure crash-report-url.txt beside the application.");
    }
    const auto bytes = readFile(config, "crash-report-url.txt", MaxUrlLength + 16);
    const std::string_view text(bytes);
    return utf8ToWide(text.starts_with("\xEF\xBB\xBF") ? text.substr(3) : text);
}

Endpoint parseEndpoint(std::wstring text, bool signedUpload = false) {
    const auto first = text.find_first_not_of(L" \r\n\t");
    if (first == std::wstring::npos) throw std::runtime_error("The crash report destination is empty.");
    text = text.substr(first, text.find_last_not_of(L" \r\n\t") - first + 1);
    if (text.size() > (signedUpload ? MaxSignedUrlLength : MaxUrlLength) ||
        std::ranges::any_of(text, [](wchar_t c) { return c <= 0x20 || c >= 0x7f; }) ||
        text.find_first_of(signedUpload ? L"\\#" : L"\\?#") != std::wstring::npos) {
        throw std::runtime_error("Use an escaped URL without whitespace, a query or a fragment.");
    }
    const auto delimiter = text.find(L"://");
    if (delimiter == std::wstring::npos) throw std::runtime_error("Use an HTTP(S) URL.");
    const auto authority = std::wstring_view(text).substr(delimiter + 3);
    if (authority.substr(0, authority.find(L'/')).find(L'@') != std::wstring_view::npos) {
        throw std::runtime_error("The crash report destination must not contain credentials.");
    }

    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = DWORD(-1);
    parts.dwUserNameLength = DWORD(-1);
    parts.dwPasswordLength = DWORD(-1);
    parts.dwUrlPathLength = DWORD(-1);
    parts.dwExtraInfoLength = DWORD(-1);
    if (!WinHttpCrackUrl(text.c_str(), static_cast<DWORD>(text.size()), 0, &parts) ||
        !parts.dwHostNameLength || parts.dwUserNameLength || parts.dwPasswordLength || !parts.nPort) {
        throw std::runtime_error("The crash report destination must be an HTTP(S) URL without credentials.");
    }
    std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    const bool secure = parts.nScheme == INTERNET_SCHEME_HTTPS;
    if (!secure && (parts.nScheme != INTERNET_SCHEME_HTTP || host != L"127.0.0.1")) {
        throw std::runtime_error("Use HTTPS or HTTP on 127.0.0.1.");
    }
    std::wstring path = parts.dwUrlPathLength ? std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength) : L"/";
    if (parts.dwExtraInfoLength) path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    return {std::move(text), std::move(host), std::move(path), parts.nPort, secure};
}

Endpoint loadEndpoint(const fs::path& exe) {
    return parseEndpoint(readUrl(exe));
}

fs::path openReportDirectory(const fs::path& directory) {
    auto resolved = fs::absolute(directory).lexically_normal();
    if (!resolved.has_filename() && resolved.has_relative_path()) resolved = resolved.parent_path();
    const auto attributes = GetFileAttributesW(resolved.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        throw std::runtime_error("Could not open the crash report folder.");
    }
    return resolved;
}

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool isReportId(std::string_view id) {
    if (id.size() != 36) return false;
    for (std::size_t index = 0; index < id.size(); ++index) {
        const bool dash = index == 8 || index == 13 || index == 18 || index == 23;
        if (dash ? id[index] != '-' : !isHex(id[index])) return false;
    }
    return true;
}

bool isUtcTimestamp(std::string_view utc) {
    if (utc.size() != 24 || utc[4] != '-' || utc[7] != '-' || utc[10] != 'T' ||
        utc[13] != ':' || utc[16] != ':' || utc[19] != '.' || utc[23] != 'Z') return false;
    const auto number = [&](std::size_t start, std::size_t length) -> unsigned {
        unsigned result{};
        for (const char c : utc.substr(start, length)) {
            if (c < '0' || c > '9') return 10000;
            result = result * 10 + static_cast<unsigned>(c - '0');
        }
        return result;
    };
    using namespace std::chrono;
    const auto y = number(0, 4);
    const auto m = number(5, 2);
    const auto d = number(8, 2);
    const auto date = year{static_cast<int>(y)} / month{m} / day{d};
    return y > 0 && y <= 9999 && m <= 12 && d <= 31 && date.ok() && number(11, 2) < 24 &&
        number(14, 2) < 60 && number(17, 2) <= 60 && number(20, 3) < 1000;
}

std::string validatedReportId(std::string_view text, const fs::path& directory) {
    const auto metadata = json::parse(text, nullptr, false);
    if (!metadata.is_object()) throw std::runtime_error("Malformed report.json.");
    const auto stringField = [&](const char* name) -> std::string_view {
        const auto field = metadata.find(name);
        return field != metadata.end() && field->is_string() ? field->get_ref<const std::string&>() : std::string_view{};
    };
    const auto thread = metadata.find("threadId");
    const auto process = metadata.find("processId");
    const auto schema = metadata.find("schemaVersion");
    const auto validIdNumber = [&](json::const_iterator field) {
        return field != metadata.end() && field->is_number_unsigned() && *field > 0 &&
            *field <= std::numeric_limits<DWORD>::max();
    };
    const auto reportId = stringField("reportId");
    const auto buildId = stringField("buildId");
    const auto commitSha = stringField("commitSha");
    const auto utc = stringField("utc");
    const auto code = stringField("exceptionCode");
    const auto alnum = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    };
    if (schema == metadata.end() || !schema->is_number_integer() || *schema != 1 || !isReportId(reportId) ||
        buildId.empty() || buildId.size() > 80 || !alnum(buildId.front()) ||
        !std::ranges::all_of(buildId, [&](char c) { return alnum(c) || c == '.' || c == '_' || c == '-'; }) ||
        (commitSha != "unknown" && (commitSha.size() != 40 ||
            !std::ranges::all_of(commitSha, [](char c) { return isHex(c) && !(c >= 'A' && c <= 'F'); }))) ||
        !isUtcTimestamp(utc) || code.size() != 10 || !code.starts_with("0x") ||
        !std::ranges::all_of(code.substr(2), &isHex) || !validIdNumber(thread) || !validIdNumber(process) ||
        !metadata.contains("dumpWritten") || metadata["dumpWritten"] != true ||
        !metadata.contains("logWritten") || metadata["logWritten"] != true) {
        throw std::runtime_error("Malformed or incomplete report.json.");
    }
    if (directory.filename().native() != std::wstring(reportId.begin(), reportId.end())) {
        throw std::runtime_error("The report ID does not match its folder name.");
    }
    return std::string(reportId);
}

Bundle packageReport(const fs::path& directory) {
    auto metadata = readFile(directory / L"report.json", "report.json", MaxMetadataBytes);
    auto reportId = validatedReportId(metadata, directory);
    auto log = readFile(directory / L"engine.log", "engine.log", MaxArchiveBytes - metadata.size());
    auto dump = readFile(directory / L"crash.dmp", "crash.dmp", MaxArchiveBytes - metadata.size() - log.size());
    if (dump.size() < 32 || !dump.starts_with("MDMP")) throw std::runtime_error("Invalid crash.dmp.");
    const std::array<core::NewFileContents, 3> files{{
        {L"report.json", std::move(metadata)}, {L"engine.log", std::move(log)}, {L"crash.dmp", std::move(dump)}
    }};
    auto zip = core::encodeStoredZip(files);
    if (zip.size() > MaxArchiveBytes) throw std::runtime_error("The report exceeds the 64 MiB limit.");
    return {std::move(reportId), std::move(zip)};
}

[[noreturn]] void failWinHttp(const char* action) {
    const auto error = GetLastError();
    throw std::runtime_error(std::string(action) + " (WinHTTP " + std::to_string(error) + ").");
}

HttpResponse requestUpload(HINTERNET session, const Endpoint& endpoint, const wchar_t* method,
        const wchar_t* headers, std::string_view body) {
    HttpResponse response;
    response.connection.reset(WinHttpConnect(session, endpoint.host.c_str(), endpoint.port, 0));
    if (!response.connection) failWinHttp("Could not connect");
    const auto flags = endpoint.secure ? WINHTTP_FLAG_SECURE : 0;
    response.request.reset(WinHttpOpenRequest(response.connection.get(), method, endpoint.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags | WINHTTP_FLAG_ESCAPE_DISABLE));
    if (!response.request) failWinHttp("Could not create the upload request");
    DWORD disabledFeatures = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_REDIRECTS;
    DWORD responseTimeout = 30000;
    if (!WinHttpSetOption(response.request.get(), WINHTTP_OPTION_DISABLE_FEATURE, &disabledFeatures, sizeof(disabledFeatures)) ||
        !WinHttpSetOption(response.request.get(), WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &responseTimeout, sizeof(responseTimeout))) {
        failWinHttp("Could not apply upload settings");
    }
    const auto count = static_cast<DWORD>(body.size());
    if (!WinHttpSendRequest(response.request.get(), headers, DWORD(-1), const_cast<char*>(body.data()),
            count, count, 0) || !WinHttpReceiveResponse(response.request.get(), nullptr)) {
        failWinHttp("Could not send the crash report");
    }
    DWORD size = sizeof(response.status);
    if (!WinHttpQueryHeaders(response.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &size, WINHTTP_NO_HEADER_INDEX)) {
        failWinHttp("Could not read the upload response");
    }
    return response;
}

std::wstring readResponseHeader(HINTERNET request, const wchar_t* name) {
    DWORD index = 0;
    std::array<wchar_t, MaxSignedUrlLength + 1> reply{};
    DWORD size = sizeof(reply);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, reply.data(), &size, &index) || !size) {
        throw std::runtime_error("The server returned an invalid upload confirmation.");
    }
    std::wstring value(reply.data(), size / sizeof(wchar_t));
    size = sizeof(reply);
    if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, name, reply.data(), &size, &index) ||
        GetLastError() != ERROR_WINHTTP_HEADER_NOT_FOUND) {
        throw std::runtime_error("The server returned more than one upload confirmation.");
    }
    return value;
}

[[noreturn]] void failStatus(DWORD status) {
    throw std::runtime_error("The server returned HTTP " + std::to_string(status) + ".");
}

void verifyReceipt(const HttpResponse& response, std::string_view reportId) {
    if (response.status != 200 && response.status != 201) failStatus(response.status);
    if (readResponseHeader(response.request.get(), ReportIdHeader) != std::wstring(reportId.begin(), reportId.end())) {
        throw std::runtime_error("The server did not confirm the expected report ID.");
    }
}

std::string sha256(std::string_view bytes) {
    std::array<UCHAR, 32> digest{};
    const auto data = reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data()));
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, data, static_cast<ULONG>(bytes.size()),
            digest.data(), static_cast<ULONG>(digest.size())) != 0) {
        throw std::runtime_error("Could not hash the ZIP for upload.");
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string hex;
    hex.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        hex.push_back(digits[byte >> 4]);
        hex.push_back(digits[byte & 15]);
    }
    return hex;
}

Endpoint loadSignedUploadUrl(HINTERNET request, const Bundle& bundle, const std::string& digest) {
    auto upload = parseEndpoint(readResponseHeader(request, UploadUrlHeader), true);
    std::wstring id(bundle.reportId.begin(), bundle.reportId.end());
    std::ranges::transform(id, id.begin(), [](wchar_t c) { return c >= L'A' && c <= L'F' ? c - L'A' + L'a' : c; });
    const auto expectedPath = L"/api/blob/?pathname=pending%2F" + id + L"-" +
        std::wstring(digest.begin(), digest.end()) + L".zip&";
    if (!upload.secure || upload.port != INTERNET_DEFAULT_HTTPS_PORT || upload.host != L"vercel.com" ||
        !upload.path.starts_with(expectedPath) || upload.path.find(L"&pathname=", expectedPath.size() - 1) != std::wstring::npos) {
        throw std::runtime_error("The server returned an invalid private upload URL.");
    }
    return upload;
}

bool postLargeReport(HINTERNET session, const Endpoint& endpoint, const Bundle& bundle) {
    const auto digest = sha256(bundle.zip);
    json metadata{{"stage", "prepare"}, {"reportId", bundle.reportId}, {"size", bundle.zip.size()}, {"sha256", digest}};
    const auto prepared = requestUpload(session, endpoint, L"POST", JsonContentType, metadata.dump());
    if (prepared.status == 404 || prepared.status == 405 || prepared.status == 415 || prepared.status == 501) return false;
    if (prepared.status != 202) {
        verifyReceipt(prepared, bundle.reportId);
        return true;
    }

    const auto upload = loadSignedUploadUrl(prepared.request.get(), bundle, digest);
    const auto stored = requestUpload(session, upload, L"PUT", ZipContentType, bundle.zip);
    if ((stored.status < 200 || stored.status >= 300) && stored.status != 409) failStatus(stored.status);
    metadata["stage"] = "complete";
    const auto completed = requestUpload(session, endpoint, L"POST", JsonContentType, metadata.dump());
    verifyReceipt(completed, bundle.reportId);
    return true;
}

void postReport(const Endpoint& endpoint, const Bundle& bundle) {
    const HttpHandle session(WinHttpOpen(L"TurboGameEngine Crash Reporter/1", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session || !WinHttpSetTimeouts(session.get(), 5000, 5000, 30000, 30000)) {
        failWinHttp("Could not start HTTP");
    }
    if (bundle.zip.size() > MaxPostBytes && endpoint.secure && postLargeReport(session.get(), endpoint, bundle)) return;
    const auto response = requestUpload(session.get(), endpoint, L"POST", ZipContentType, bundle.zip);
    verifyReceipt(response, bundle.reportId);
}

SendResult sendReport(const Endpoint& endpoint, const fs::path& directory) {
    try {
        auto report = packageReport(openReportDirectory(directory));
        postReport(endpoint, report);
        return {std::move(report.reportId), {}};
    } catch (const std::exception& error) {
        return {{}, error.what()};
    }
}

struct DialogState {
    const fs::path& directory;
    const std::optional<Endpoint>& endpoint;
    std::thread worker;
    std::atomic<bool> finished{};
    SendResult result;
    bool hadError{};
    bool workerFailed{};
};

void footer(HWND dialog, const std::wstring& text) {
    SendMessageW(dialog, TDM_SET_ELEMENT_TEXT, TDE_FOOTER, reinterpret_cast<LPARAM>(text.c_str()));
}

void buttons(HWND dialog, bool sending, bool canSend) {
    SendMessageW(dialog, TDM_ENABLE_BUTTON, SendButton, canSend && !sending);
    SendMessageW(dialog, TDM_ENABLE_BUTTON, OpenFolderButton, !sending);
    SendMessageW(dialog, TDM_ENABLE_BUTTON, IDCANCEL, !sending);
}

HRESULT CALLBACK dialogCallback(HWND dialog, UINT notification, WPARAM button, LPARAM, LONG_PTR data) noexcept {
    auto& state = *reinterpret_cast<DialogState*>(data);
    try {
        if (notification == TDN_CREATED) buttons(dialog, false, state.endpoint.has_value());
        if (notification == TDN_BUTTON_CLICKED && button == SendButton) {
            if (!state.endpoint || state.worker.joinable() || !state.result.reportId.empty()) return S_FALSE;
            state.finished.store(false, std::memory_order_relaxed);
            state.workerFailed = false;
            state.worker = std::thread([&state] {
                try { state.result = sendReport(*state.endpoint, state.directory); }
                catch (...) { state.workerFailed = true; }
                state.finished.store(true, std::memory_order_release);
            });
            buttons(dialog, true, false);
            footer(dialog, L"Sending report…");
            return S_FALSE;
        }
        if (notification == TDN_BUTTON_CLICKED && button == OpenFolderButton) {
            if (!state.worker.joinable()) {
                (void)openReportDirectory(state.directory);
                const auto opened = ShellExecuteW(dialog, L"open", state.directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(opened) <= 32) footer(dialog, L"Could not open the report folder.");
            }
            return S_FALSE;
        }
        if (notification == TDN_BUTTON_CLICKED && state.worker.joinable()) return S_FALSE;
        if (notification == TDN_TIMER && state.worker.joinable() && state.finished.load(std::memory_order_acquire)) {
            state.worker.join();
            state.hadError = state.workerFailed || !state.result.error.empty();
            buttons(dialog, false, state.hadError);
            if (state.hadError) {
                const auto message = state.workerFailed ? L"Could not send the report." : utf8ToWide(state.result.error);
                footer(dialog, message + L" Choose Send Report to retry.");
            } else {
                std::printf("Report sent: %s\n", state.result.reportId.c_str());
                footer(dialog, L"Report sent: " + utf8ToWide(state.result.reportId));
            }
        }
        return S_OK;
    } catch (const std::exception& error) {
        state.hadError = true;
        const bool sending = state.worker.joinable();
        buttons(dialog, sending, !sending && state.endpoint.has_value() && state.result.reportId.empty());
        try { footer(dialog, utf8ToWide(error.what())); } catch (...) { footer(dialog, L"Could not send the report."); }
        return S_FALSE;
    }
}

int showReportDialog(const fs::path& directory, const std::optional<Endpoint>& endpoint, std::string_view error) {
    DialogState state{directory, endpoint, {}, {}, {}, !error.empty(), false};
    const std::wstring destination = endpoint ? L"Destination\n" + endpoint->url + L"\n\n" : L"Destination unavailable.\n\n";
    const auto description = destination +
        L"The ZIP includes the crash dump (crash.dmp), the engine log (engine.log), and the report details "
        L"(report.json). The report may contain process memory and local file paths.";
    const auto status = error.empty() ? std::wstring(L"Your report is saved locally.") : utf8ToWide(error);
    const TASKDIALOG_BUTTON actions[] = {
        {SendButton, L"Send Report"}, {OpenFolderButton, L"Open Folder"}, {IDCANCEL, L"Close"}
    };
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER;
    dialog.pszWindowTitle = L"TurboGameEngine Crash Report";
    dialog.pszMainIcon = TD_WARNING_ICON;
    dialog.pszMainInstruction = L"TurboGameEngine closed unexpectedly";
    dialog.pszContent = description.c_str();
    dialog.cButtons = static_cast<UINT>(std::size(actions));
    dialog.pButtons = actions;
    dialog.nDefaultButton = IDCANCEL;
    dialog.pszFooter = status.c_str();
    dialog.pfCallback = &dialogCallback;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(&state);
    const auto result = TaskDialogIndirect(&dialog, nullptr, nullptr, nullptr);
    if (state.worker.joinable()) state.worker.join();
    if (FAILED(result)) throw std::runtime_error("Could not open the crash report dialog.");
    return state.hadError ? 1 : 0;
}

}

void enableCrashReportDialog() noexcept {
    try {
        const auto exe = executablePath();
        (void)loadEndpoint(exe);
        core::CrashReport::enableDialog(exe);
    } catch (...) {
    }
}

int runCrashReportClient(const std::filesystem::path& reportDirectory, bool sendOnly) noexcept {
    try {
        const auto directory = openReportDirectory(reportDirectory);
        std::optional<Endpoint> endpoint;
        std::string configurationError;
        try { endpoint = loadEndpoint(executablePath()); }
        catch (const std::exception& error) { configurationError = error.what(); }
        if (!sendOnly) return showReportDialog(directory, endpoint, configurationError);
        if (!endpoint) throw std::runtime_error(configurationError);
        const auto result = sendReport(*endpoint, directory);
        if (!result.error.empty()) throw std::runtime_error(result.error);
        std::printf("Report sent: %s\n", result.reportId.c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Crash report: %s\n", error.what());
        if (!sendOnly) {
            try {
                MessageBoxW(nullptr, utf8ToWide(error.what()).c_str(), L"TurboGameEngine Crash Report", MB_OK | MB_ICONERROR);
            } catch (...) {
                MessageBoxW(nullptr, L"Could not process the report.", L"TurboGameEngine Crash Report", MB_OK | MB_ICONERROR);
            }
        }
        return 1;
    }
}

}
