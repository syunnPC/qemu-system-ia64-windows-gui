// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Updates.h"
#include "Localization.h"

#include <Winhttp.h>
#include <bcrypt.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <format>
#include <memory>

namespace Ia64
{
namespace
{
constexpr std::wstring_view Github = L"https://github.com/syunnPC/qemu-system-ia64";
constexpr std::wstring_view Api = L"https://api.github.com/repos/syunnPC/qemu-system-ia64";
constexpr std::wstring_view Nightly = L"https://nightly.link/syunnPC/qemu-system-ia64";
constexpr std::string_view WindowsZip = "qemu-system-ia64-windows-x86_64.zip";
constexpr std::string_view OptimizedWindowsZip = "qemu-system-ia64-windows-x86_64-v2-optimized.zip";
constexpr std::string_view FirmwareZip = "qemu-system-ia64-firmware.zip";
constexpr std::string_view FirmwareBin = "ia64-firmware.bin";
constexpr uint64_t MaxAssetSize = 512ull * 1024 * 1024;

bool SupportsOptimizedWindows()
{
    int registers[4]{};
    __cpuid(registers, 1);
    const auto features = static_cast<unsigned>(registers[2]);
    // CPUID.01H:ECX: SSE3, SSSE3, CMPXCHG16B, SSE4.1, SSE4.2, POPCNT.
    constexpr unsigned v2Features = (1u << 0) | (1u << 9) | (1u << 13) | (1u << 19) | (1u << 20) | (1u << 23);
    if ((features & v2Features) != v2Features)
    {
        return false;
    }
    __cpuid(registers, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(registers[0]) < 0x80000001u)
    {
        return false;
    }
    __cpuid(registers, static_cast<int>(0x80000001u));
    return (registers[2] & 1) != 0; // LAHF/SAHF in 64-bit mode.
}

struct CloseHttp
{
    void operator()(void* handle) const
    {
        WinHttpCloseHandle(handle);
    }
};
using HttpHandle = std::unique_ptr<void, CloseHttp>;

class Request
{
public:
    explicit Request(const std::wstring& url, const wchar_t* method = L"GET")
        : _session(WinHttpOpen(L"QEMU IA-64 Manager/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0))
    {
        if (!_session)
        {
            WinError(LText(IDS_HttpOpen));
        }
        WinHttpSetTimeouts(_session.get(), 8000, 8000, 20000, 20000);
        URL_COMPONENTSW parts{};
        parts.dwStructSize = sizeof(parts);
        parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwExtraInfoLength = DWORD(-1);
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS)
        {
            Fail(LText(IDS_InvalidUrl));
        }
        const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
        auto resource = std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
        if (parts.dwExtraInfoLength)
        {
            resource.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
        }
        _connection.reset(WinHttpConnect(_session.get(), host.c_str(), parts.nPort, 0));
        if (!_connection)
        {
            WinError(LFmt(IDS_HttpConnect, host));
        }
        _request.reset(WinHttpOpenRequest(_connection.get(), method, resource.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
        if (!_request)
        {
            WinError(LText(IDS_HttpOpenRequest));
        }
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        if (!WinHttpSetOption(_request.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect)))
        {
            WinError(LText(IDS_HttpPolicy));
        }
        if (!WinHttpSendRequest(_request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
            !WinHttpReceiveResponse(_request.get(), nullptr))
        {
            WinError(LFmt(IDS_HttpRequest, host));
        }
        DWORD size = sizeof(_status);
        if (!WinHttpQueryHeaders(_request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &_status, &size, WINHTTP_NO_HEADER_INDEX))
        {
            WinError(LText(IDS_HttpStatusRead));
        }
    }

    unsigned Status() const
    {
        return _status;
    }

    std::wstring Header(DWORD name) const
    {
        DWORD size{};
        WinHttpQueryHeaders(_request.get(), name, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
        {
            return {};
        }
        std::wstring value(size / sizeof(wchar_t), L'\0');
        if (!WinHttpQueryHeaders(_request.get(), name, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
                                 WINHTTP_NO_HEADER_INDEX))
        {
            WinError(LText(IDS_HttpHeader));
        }
        value.resize(size / sizeof(wchar_t));
        return value;
    }

    size_t Read(std::span<char> buffer) const
    {
        DWORD count{};
        if (!WinHttpReadData(_request.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count))
        {
            WinError(LText(IDS_HttpRead));
        }
        return count;
    }

private:
    HttpHandle _session;
    HttpHandle _connection;
    HttpHandle _request;
    DWORD _status{};
};

void RequireHttp(const Request& request, std::wstring_view description)
{
    if (request.Status() != 200)
    {
        Fail(LFmt(IDS_HttpStatus, description, request.Status()));
    }
}

Json Metadata(std::wstring_view suffix)
{
    Request request(std::wstring(Api) + std::wstring(suffix));
    RequireHttp(request, L"GitHub API");
    std::array<char, 16384> buffer{};
    std::string body;
    while (const auto count = request.Read(buffer))
    {
        body.append(buffer.data(), count);
        if (body.size() > 3 * 1024 * 1024)
        {
            Fail(LText(IDS_MetadataSize));
        }
    }
    return Json::parse(body);
}

class Sha256
{
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(&_algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        {
            Fail(LText(IDS_HashOpen));
        }
        if (BCryptCreateHash(_algorithm, &_hash, nullptr, 0, nullptr, 0, 0) < 0)
        {
            BCryptCloseAlgorithmProvider(_algorithm, 0);
            Fail(LText(IDS_HashCreate));
        }
    }

    ~Sha256()
    {
        BCryptDestroyHash(_hash);
        BCryptCloseAlgorithmProvider(_algorithm, 0);
    }

    void Update(std::span<char> buffer)
    {
        if (BCryptHashData(_hash, reinterpret_cast<PUCHAR>(buffer.data()), static_cast<ULONG>(buffer.size()), 0) < 0)
        {
            Fail(LText(IDS_HashData));
        }
    }

    std::string Finish() const
    {
        std::array<unsigned char, 32> bytes{};
        if (BCryptFinishHash(_hash, bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0)
        {
            Fail(LText(IDS_HashFinish));
        }
        std::string result = "sha256:";
        for (const auto value : bytes)
        {
            result += std::format("{:02x}", value);
        }
        return result;
    }

private:
    BCRYPT_ALG_HANDLE _algorithm{};
    BCRYPT_HASH_HANDLE _hash{};
};

struct Asset
{
    std::wstring Url;
    std::string Digest;
    uint64_t Size{};
};

struct Package
{
    std::string Identity;
    Asset Windows;
    std::optional<Asset> Firmware;
    bool FirmwareArchive{};
};

std::string Digest(const Json& asset)
{
    const auto found = asset.find("digest");
    if (found == asset.end() || found->is_null())
    {
        return {};
    }
    const auto digest = found->get<std::string>();
    if (digest.size() != 71 || !digest.starts_with("sha256:") ||
        digest.find_first_not_of("0123456789abcdef", 7) != std::string::npos)
    {
        Fail(LText(IDS_HashFormat));
    }
    return digest;
}

std::optional<Package> Stable(bool optimizedHost)
{
    const auto probe = [](std::string_view name, std::wstring& etag) -> std::optional<Asset> {
        auto url = std::format(L"{}/releases/latest/download/{}", Github, Wide(name));
        Request request(url, L"HEAD");
        if (request.Status() == 404)
        {
            return std::nullopt;
        }
        RequireHttp(request, LText(IDS_StableRelease));
        etag = request.Header(WINHTTP_QUERY_ETAG);
        return Asset{std::move(url), {}, 0};
    };
    std::wstring etag, firmwareEtag;
    auto name = optimizedHost ? OptimizedWindowsZip : WindowsZip;
    auto windows = probe(name, etag);
    if (!windows && optimizedHost)
    {
        name = WindowsZip;
        windows = probe(name, etag);
    }
    if (!windows)
    {
        return std::nullopt;
    }
    auto firmware = probe(FirmwareBin, firmwareEtag);
    return Package{etag.empty() ? "stable:" : std::format("stable:{}:{}:{}", name, Utf8(etag), Utf8(firmwareEtag)),
                   std::move(*windows), std::move(firmware), false};
}

const Json* SelectReleaseWindows(const Json& assets, bool optimizedHost)
{
    const Json* selected = nullptr;
    int priority = 0;
    bool ambiguous = false;
    for (const auto& asset : assets)
    {
        const auto& name = asset.at("name").get_ref<const std::string&>();
        if (!name.starts_with("qemu-system-ia64") || !name.ends_with(".zip") ||
            !(name.contains("win64") || name.contains("windows-ucrt64") || name.contains("windows-x86_64")))
        {
            continue;
        }
        const bool optimized = name.ends_with("-optimized.zip") || name.contains("-optimized-");
        if (optimized && !optimizedHost)
        {
            continue;
        }
        const int rank = optimized ? (name == OptimizedWindowsZip ? 4 : 3) : (name == WindowsZip ? 2 : 1);
        if (rank > priority)
        {
            priority = rank;
            selected = &asset;
            ambiguous = false;
        }
        else if (rank == priority)
        {
            ambiguous = true;
        }
    }
    if (ambiguous)
    {
        Fail(LText(IDS_ReleaseAmbiguous));
    }
    return selected;
}

Package BetaStable(bool optimizedHost)
{
    const auto releases = Metadata(L"/releases?per_page=30");
    const Json* newest = nullptr;
    for (const auto& release : releases)
    {
        if (release.value("draft", true) || release.at("published_at").is_null())
        {
            continue;
        }
        if (!newest || release.at("published_at") > newest->at("published_at"))
        {
            newest = &release;
        }
    }
    if (!newest)
    {
        Fail(LText(IDS_BetaEmpty));
    }
    const auto read = [](const Json& asset) {
        const auto url = Wide(asset.at("browser_download_url").get<std::string>());
        if (!url.starts_with(std::wstring(Github) + L"/releases/download/"))
        {
            Fail(LText(IDS_ReleaseUrl));
        }
        return Asset{url, Digest(asset), asset.at("size").get<uint64_t>()};
    };
    const auto& assets = newest->at("assets");
    const auto windows = SelectReleaseWindows(assets, optimizedHost);
    if (!windows)
    {
        Fail(LText(IDS_ReleaseNoWindows));
    }
    std::optional<Asset> firmware;
    for (const auto& value : assets)
    {
        if (value.at("name").get_ref<const std::string&>() == FirmwareBin)
        {
            firmware = read(value);
        }
    }
    auto archive = read(*windows);
    auto identity = std::format("beta-stable:{}:{}:{}", newest->at("tag_name").get<std::string>(),
                                windows->at("name").get<std::string>(), archive.Digest);
    if (firmware)
    {
        identity += ":" + firmware->Digest;
    }
    return {std::move(identity), std::move(archive), std::move(firmware), false};
}

Package Experimental(bool optimizedHost)
{
    const auto runs = Metadata(L"/actions/workflows/build.yml/runs?branch=main&status=success&per_page=1");
    if (runs.at("workflow_runs").empty())
    {
        Fail(LText(IDS_ActionsEmpty));
    }
    const auto& run = runs.at("workflow_runs").front();
    const auto id = run.at("id").get<uint64_t>();
    const auto suite = run.at("check_suite_id").get<uint64_t>();
    const auto response = Metadata(std::format(L"/actions/runs/{}/artifacts?per_page=100", id));
    const auto& artifacts = response.at("artifacts");
    const auto read = [&](std::string_view name) -> std::optional<Asset> {
        const auto found = std::ranges::find_if(artifacts, [&](const Json& asset) {
            return asset.at("name").get<std::string>() == name && !asset.value("expired", true);
        });
        if (found == artifacts.end())
        {
            return std::nullopt;
        }
        const auto digest = Digest(*found);
        if (digest.empty())
        {
            Fail(LText(IDS_ActionsHash));
        }
        return Asset{std::format(L"{}/suites/{}/artifacts/{}", Nightly, suite, found->at("id").get<uint64_t>()), digest,
                     found->at("size_in_bytes").get<uint64_t>()};
    };
    auto windows = optimizedHost ? read(OptimizedWindowsZip) : std::nullopt;
    if (!windows)
    {
        windows = read(WindowsZip);
    }
    if (!windows)
    {
        Fail(LText(IDS_ActionsNoWindows));
    }
    auto firmware = read(FirmwareZip);
    return {"experimental:" + std::to_string(id) + ":" + windows->Digest + (firmware ? ":" + firmware->Digest : ""),
            std::move(*windows), std::move(firmware), true};
}

void Download(const Asset& asset, const Fs::path& path)
{
    Request request(asset.Url);
    RequireHttp(request, LText(IDS_AssetDownload));
    Handle file(CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!file)
    {
        WinError(LText(IDS_DownloadFile));
    }
    Sha256 hash;
    std::array<char, 64 * 1024> buffer{};
    uint64_t size{};
    const auto deadline = std::chrono::steady_clock::now() + 5min;
    while (const auto count = request.Read(buffer))
    {
        size += count;
        if (size > MaxAssetSize || (asset.Size && size > asset.Size) || std::chrono::steady_clock::now() >= deadline)
        {
            Fail(LText(IDS_DownloadLimits));
        }
        hash.Update(std::span(buffer).first(count));
        WriteAll(file.Get(), std::string_view(buffer.data(), count));
    }
    if (!size || (asset.Size && asset.Size != size) || (!asset.Digest.empty() && asset.Digest != hash.Finish()))
    {
        Fail(LText(IDS_DownloadMismatch));
    }
}

bool IsReparsePoint(const Fs::path& path)
{
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

Fs::path FindFile(const Fs::path& directory, const wchar_t* filename)
{
    Fs::path found;
    for (const auto& entry : Fs::recursive_directory_iterator(directory))
    {
        if (IsReparsePoint(entry.path()))
        {
            Fail(LText(IDS_ArchiveReparse));
        }
        if (entry.path().filename() == filename && entry.is_regular_file())
        {
            if (!found.empty())
            {
                Fail(LText(IDS_ArchiveDuplicate));
            }
            found = entry.path();
        }
    }
    return found;
}

void Extract(const Fs::path& archive, const Fs::path& directory)
{
    Fs::create_directories(directory);
    const std::array args{std::wstring(L"-xf"),
                          archive.native(),
                          std::wstring(L"-C"),
                          directory.native(),
                          std::wstring(L"--no-same-owner"),
                          std::wstring(L"--no-same-permissions")};
    RequireSuccess(Run(SystemExecutable(L"tar.exe"), args, 90s), LText(IDS_ArchiveExtract));
}

class Staging
{
public:
    explicit Staging(Fs::path directory) : Path(std::move(directory))
    {
        Fs::create_directories(Path);
    }
    ~Staging()
    {
        std::error_code error;
        Fs::remove_all(Path, error);
    }

    const Fs::path Path;
};
} // namespace

std::wstring_view ChannelName(Channel channel)
{
    switch (channel)
    {
    case Channel::Stable:
        return L"stable";
    case Channel::BetaStable:
        return L"beta-stable";
    case Channel::Experimental:
        return L"experimental";
    }
    return {};
}

std::optional<Engine> UpdateEngine(Channel channel, const Engine& current)
{
    const bool optimizedHost = SupportsOptimizedWindows();
    const auto available =
        channel == Channel::Stable
            ? Stable(optimizedHost)
            : std::optional(channel == Channel::BetaStable ? BetaStable(optimizedHost) : Experimental(optimizedHost));
    if (!available)
    {
        return std::nullopt;
    }
    const auto& package = *available;
    if (package.Identity == current.ReleaseTag && IsFile(current.Qemu) && IsFile(current.Firmware) &&
        (channel != Channel::Stable || package.Identity != "stable:"))
    {
        return current;
    }

    const auto runtimes = DataDirectory() / L"Runtimes";
    const auto id = NewId();
    Staging staging(runtimes / (L".download-" + id));
    const auto contents = staging.Path / L"contents";
    const auto archive = staging.Path / L"windows.zip";
    Download(package.Windows, archive);
    Extract(archive, contents);
    auto executable = FindFile(contents, L"qemu-system-ia64.exe");
    auto imageTool = FindFile(contents, L"qemu-img.exe");
    Fs::path firmware;
    if (channel != Channel::Experimental || !package.Firmware)
    {
        firmware = FindFile(contents, L"ia64-firmware.bin");
    }
    if (executable.empty())
    {
        Fail(LText(IDS_ArchiveNoEmulator));
    }
    if (firmware.empty() && package.Firmware)
    {
        const auto firmwareFolder = contents / L"firmware";
        Fs::create_directories(firmwareFolder);
        if (package.FirmwareArchive)
        {
            const auto firmwareArchive = staging.Path / L"firmware.zip";
            Download(*package.Firmware, firmwareArchive);
            Extract(firmwareArchive, firmwareFolder);
            firmware = FindFile(firmwareFolder, L"ia64-firmware.bin");
        }
        else
        {
            firmware = firmwareFolder / L"ia64-firmware.bin";
            Download(*package.Firmware, firmware);
        }
    }
    if (firmware.empty())
    {
        Fail(LText(channel == Channel::Experimental ? IDS_ActionsNoFirmwareZip : IDS_ArchiveNoFirmware));
    }
    if (channel == Channel::Stable && package.Identity != "stable:")
    {
        const auto latest = Stable(optimizedHost);
        if (!latest || latest->Identity != package.Identity)
        {
            Fail(LText(IDS_StableChanged));
        }
    }
    const auto install = runtimes / id;
    executable = executable.lexically_relative(contents);
    firmware = firmware.lexically_relative(contents);
    if (!imageTool.empty())
    {
        imageTool = imageTool.lexically_relative(contents);
    }
    RequireSuccess(Run(contents / executable, std::array{std::wstring(L"--version")}), LText(IDS_DownloadCheck));
    Fs::rename(contents, install);
    return Engine{install / executable, imageTool.empty() ? current.ImageTool : install / imageTool, install / firmware,
                  package.Identity};
}
} // namespace Ia64
