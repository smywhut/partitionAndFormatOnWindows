#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr wchar_t kBasicDataPartitionGuid[] = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";

struct FormatOptions {
    std::wstring fs;
    std::wstring volLabel;
    bool quick{ true };
};

struct PartitionRequest {
    std::optional<ULONGLONG> offsetBytes;
    ULONGLONG sizeBytes{ 0 };
    std::wstring partLabel;
    std::wstring partType{ kBasicDataPartitionGuid };
    std::optional<FormatOptions> format;
};

struct CliOptions {
    DWORD diskNumber{ 0 };
    bool gpt{ false };
    std::vector<PartitionRequest> partitions;
};

std::string WideToUtf8(std::wstring_view input) {
    if (input.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "<utf8-convert-failed>";
    }

    std::string result(static_cast<size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, input.data(), static_cast<int>(input.size()), result.data(), size, nullptr, nullptr);
    if (written != size) {
        return "<utf8-convert-failed>";
    }
    return result;
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

std::wstring Trim(std::wstring s) {
    const auto isSpace = [](wchar_t ch) { return std::iswspace(ch) != 0; };
    const auto begin = std::find_if_not(s.begin(), s.end(), isSpace);
    const auto end = std::find_if_not(s.rbegin(), s.rend(), isSpace).base();
    if (begin >= end) {
        return L"";
    }
    return std::wstring(begin, end);
}

ULONGLONG ParseSizeString(const std::wstring& s) {
    if (s.empty()) {
        throw std::runtime_error("Empty size string.");
    }

    wchar_t* end = nullptr;
    const double value = std::wcstod(s.c_str(), &end);
    if (end == s.c_str() || value <= 0) {
        throw std::runtime_error("Invalid size value.");
    }

    ULONGLONG multiplier = 1;
    if (*end != L'\0') {
        const wchar_t unit = static_cast<wchar_t>(std::towupper(*end));
        switch (unit) {
        case L'K': multiplier = 1024ull; break;
        case L'M': multiplier = 1024ull * 1024ull; break;
        case L'G': multiplier = 1024ull * 1024ull * 1024ull; break;
        case L'T': multiplier = 1024ull * 1024ull * 1024ull * 1024ull; break;
        default: throw std::runtime_error("Unsupported size unit; use K/M/G/T.");
        }
        ++end;
        if (*end != L'\0') {
            throw std::runtime_error("Trailing characters after size unit.");
        }
    }

    return static_cast<ULONGLONG>(value * static_cast<double>(multiplier));
}

std::wstring ParseGuidOrAlias(const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    if (lower == L"basic" || lower == L"basicdata") {
        return kBasicDataPartitionGuid;
    }
    return value;
}

std::map<std::wstring, std::wstring> ParseKeyValueList(const std::wstring& raw) {
    std::map<std::wstring, std::wstring> kv;
    std::wstringstream ss(raw);
    std::wstring token;
    while (std::getline(ss, token, L',')) {
        token = Trim(token);
        const auto pos = token.find(L'=');
        if (pos == std::wstring::npos || pos == 0) {
            throw std::runtime_error("Invalid key=value token.");
        }
        std::wstring key = ToLower(Trim(token.substr(0, pos)));
        std::wstring value = Trim(token.substr(pos + 1));
        if (value.empty()) {
            throw std::runtime_error("Value in key=value cannot be empty.");
        }
        kv[key] = value;
    }
    return kv;
}

PartitionRequest ParseCreatePart(const std::wstring& raw) {
    PartitionRequest req;
    const auto kv = ParseKeyValueList(raw);

    const auto itSize = kv.find(L"size");
    if (itSize == kv.end()) {
        throw std::runtime_error("create-part requires size=...");
    }
    req.sizeBytes = ParseSizeString(itSize->second);

    if (const auto itOff = kv.find(L"offset"); itOff != kv.end()) {
        req.offsetBytes = ParseSizeString(itOff->second);
    }
    if (const auto itLabel = kv.find(L"label"); itLabel != kv.end()) {
        req.partLabel = itLabel->second;
    }
    if (const auto itType = kv.find(L"type"); itType != kv.end()) {
        req.partType = ParseGuidOrAlias(itType->second);
    }

    return req;
}

FormatOptions ParseFormat(const std::wstring& raw) {
    FormatOptions fmt;
    const auto kv = ParseKeyValueList(raw);

    const auto itFs = kv.find(L"fs");
    if (itFs == kv.end()) {
        throw std::runtime_error("format requires fs=ntfs|fat32|exfat");
    }
    fmt.fs = ToLower(itFs->second);

    if (const auto itVol = kv.find(L"vol"); itVol != kv.end()) {
        fmt.volLabel = itVol->second;
    }
    if (const auto itQuick = kv.find(L"quick"); itQuick != kv.end()) {
        fmt.quick = (itQuick->second == L"1" || ToLower(itQuick->second) == L"true");
    }
    return fmt;
}

CliOptions ParseArgs(int argc, wchar_t** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];

        if (arg.rfind(L"--disk=", 0) == 0) {
            opt.diskNumber = static_cast<DWORD>(std::stoul(arg.substr(7)));
        }
        else if (arg == L"--gpt") {
            opt.gpt = true;
        }
        else if (arg.rfind(L"--create-part", 0) == 0) {
            const auto pos = arg.find(L'=');
            if (pos == std::wstring::npos) {
                throw std::runtime_error("--create-part requires value list.");
            }
            opt.partitions.push_back(ParseCreatePart(arg.substr(pos + 1)));
        }
        else if (arg.rfind(L"--format", 0) == 0) {
            const auto pos = arg.find(L'=');
            if (pos == std::wstring::npos) {
                throw std::runtime_error("--format requires value list.");
            }
            if (opt.partitions.empty()) {
                throw std::runtime_error("--format must follow at least one --create-part.");
            }
            opt.partitions.back().format = ParseFormat(arg.substr(pos + 1));
        }
        else {
            throw std::runtime_error("Unknown argument.");
        }
    }

    if (opt.partitions.empty()) {
        throw std::runtime_error("At least one --create-part is required.");
    }
    return opt;
}

std::wstring QuotePsSingle(const std::wstring& value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'\'');
    for (wchar_t ch : value) {
        escaped.push_back(ch);
        if (ch == L'\'') {
            escaped.push_back(L'\'');
        }
    }
    escaped.push_back(L'\'');
    return escaped;
}

std::wstring ValidateFs(const std::wstring& fs) {
    if (fs == L"ntfs") {
        return L"NTFS";
    }
    if (fs == L"fat32") {
        return L"FAT32";
    }
    if (fs == L"exfat") {
        return L"exFAT";
    }
    throw std::runtime_error("Unsupported file system type.");
}

std::wstring BuildSmapiScript(const CliOptions& opt) {
    std::wstringstream script;
    script << L"$ErrorActionPreference = 'Stop'\n";
    script << L"$diskNumber = " << opt.diskNumber << L"\n";

    if (opt.gpt) {
        script << L"Initialize-Disk -Number $diskNumber -PartitionStyle GPT -Confirm:$false | Out-Null\n";
    }

    int index = 1;
    for (const auto& req : opt.partitions) {
        const std::wstring partitionVar = L"$partition" + std::to_wstring(index++);
        script << partitionVar << L" = New-Partition -DiskNumber $diskNumber -Size " << req.sizeBytes;
        if (req.offsetBytes.has_value()) {
            script << L" -Offset " << req.offsetBytes.value();
        }
        script << L" -GptType " << QuotePsSingle(req.partType) << L"\n";

        if (!req.partLabel.empty()) {
            script << L"Write-Host " << QuotePsSingle(L"[warn] SMAPI/New-Partition 不支持直接设置 GPT 分区名，已忽略 label=" + req.partLabel) << L"\n";
        }

        if (req.format.has_value()) {
            const auto& fmt = req.format.value();
            script << L"Format-Volume -Partition " << partitionVar << L" -FileSystem " << ValidateFs(fmt.fs);
            if (!fmt.volLabel.empty()) {
                script << L" -NewFileSystemLabel " << QuotePsSingle(fmt.volLabel);
            }
            script << L" -Confirm:$false -Force";
            if (!fmt.quick) {
                script << L" -Full";
            }
            script << L" | Out-Null\n";
        }
    }

    return script.str();
}

int RunPowerShell(const std::wstring& script) {
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;

    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        throw std::runtime_error("CreatePipe failed.");
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdline = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command " + QuotePsSingle(script);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(L'\0');

    if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(readPipe);
        CloseHandle(writePipe);
        throw std::runtime_error("Failed to start powershell.exe.");
    }

    CloseHandle(writePipe);

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        output.append(buffer, buffer + bytesRead);
    }

    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!output.empty()) {
        std::cout << output;
    }

    return static_cast<int>(exitCode);
}

void Usage() {
    std::wcout << L"Usage:\n"
               << L"  disk_part_fmt --disk=1 --gpt \\\n"
               << L"    --create-part=size=10G,label=MyPart,type=basic \\\n"
               << L"    --format=fs=ntfs,vol=Data,quick=1\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc <= 1) {
        Usage();
        return 1;
    }

    try {
        const auto opt = ParseArgs(argc, argv);
        const std::wstring script = BuildSmapiScript(opt);

        std::wcout << L"Using Storage Management API (SMAPI) via PowerShell Storage module...\n";
        const int exitCode = RunPowerShell(script);
        if (exitCode != 0) {
            std::wstringstream ss;
            ss << L"PowerShell exited with code " << exitCode;
            throw std::runtime_error(WideToUtf8(ss.str()));
        }

        std::wcout << L"Done.\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }
}
