#include <windows.h>
#include <vds.h>
#include <comdef.h>
#include <objbase.h>
#include <atlbase.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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

#pragma comment(lib, "vdsuuid.lib")

using ATL::CComPtr;

namespace {

constexpr GUID kBasicDataPartitionGuid =
{ 0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7} };

struct FormatOptions {
    std::wstring fs;      // NTFS/FAT32/exFAT
    std::wstring volLabel;
    bool quick{ true };
};

struct PartitionRequest {
    std::optional<ULONGLONG> offsetBytes;
    ULONGLONG sizeBytes{ 0 };
    std::wstring partLabel;
    GUID partType{ kBasicDataPartitionGuid };
    std::optional<FormatOptions> format;
};

struct CliOptions {
    DWORD diskNumber{ 0 };
    bool gpt{ false };
    std::vector<PartitionRequest> partitions;
};

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return s;
}

void CheckHr(HRESULT hr, const std::wstring& message) {
    if (FAILED(hr)) {
        _com_error err(hr);
        std::wstringstream ss;
        ss << message << L" (hr=0x" << std::hex << hr << L", " << err.ErrorMessage() << L")";
        throw std::runtime_error(std::string(ss.str().begin(), ss.str().end()));
    }
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

GUID ParseGuidOrAlias(const std::wstring& value) {
    const std::wstring lower = ToLower(value);
    if (lower == L"basic" || lower == L"basicdata") {
        return kBasicDataPartitionGuid;
    }

    GUID guid{};
    const HRESULT hr = CLSIDFromString(value.c_str(), &guid);
    CheckHr(hr, L"Invalid GUID for partition type");
    return guid;
}

std::map<std::wstring, std::wstring> ParseKeyValueList(const std::wstring& raw) {
    std::map<std::wstring, std::wstring> kv;
    std::wstringstream ss(raw);
    std::wstring token;
    while (std::getline(ss, token, L',')) {
        const auto pos = token.find(L'=');
        if (pos == std::wstring::npos || pos == 0) {
            throw std::runtime_error("Invalid key=value token.");
        }
        std::wstring key = ToLower(token.substr(0, pos));
        std::wstring value = token.substr(pos + 1);
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
    fmt.fs = itFs->second;

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

void WaitAsync(IVdsAsync* async, const std::wstring& action) {
    HRESULT hrResult = S_OK;
    VDS_ASYNC_OUTPUT output{};
    CheckHr(async->Wait(&hrResult, &output), action + L": async wait failed");
    CheckHr(hrResult, action + L": operation failed");
}

void InitializeDiskGpt(IVdsAdvancedDisk* advDisk) {
    CComPtr<IVdsAsync> async;
    CheckHr(advDisk->ConvertStyle(VDS_PST_GPT, FALSE, &async), L"Convert disk style to GPT");
    WaitAsync(async, L"Convert to GPT");
}

void BuildCreateParams(const PartitionRequest& req, CREATE_PARTITION_PARAMETERS& params) {
    ZeroMemory(&params, sizeof(params));
    params.style = VDS_PST_GPT;
    params.offset = req.offsetBytes.value_or(0);
    params.align = 1024 * 1024; // 1MiB alignment
    params.PartitionStyle.Gpt.PartitionType = req.partType;
    params.PartitionStyle.Gpt.PartitionId = GUID_NULL;
    params.PartitionStyle.Gpt.Attributes = 0;

    wcsncpy_s(params.PartitionStyle.Gpt.name,
        _countof(params.PartitionStyle.Gpt.name),
        req.partLabel.c_str(),
        _TRUNCATE);
}

void FormatVolume(IVdsVolumeMF* volumeMf, const FormatOptions& fmt) {
    VDS_FILE_SYSTEM_TYPE fsType = VDS_FST_NTFS;
    const auto lower = ToLower(fmt.fs);
    if (lower == L"ntfs") fsType = VDS_FST_NTFS;
    else if (lower == L"fat32") fsType = VDS_FST_FAT32;
    else if (lower == L"exfat") fsType = VDS_FST_EXFAT;
    else throw std::runtime_error("Unsupported file system type.");

    CComPtr<IVdsAsync> async;
    CheckHr(volumeMf->Format(fsType,
        fmt.volLabel.empty() ? nullptr : const_cast<LPWSTR>(fmt.volLabel.c_str()),
        VDS_FPF_NONE,
        0,
        fmt.quick,
        0,
        &async), L"Volume format");

    WaitAsync(async, L"Format volume");
}

CComPtr<IVdsService> ConnectVdsService() {
    CComPtr<IVdsServiceLoader> loader;
    CheckHr(CoCreateInstance(CLSID_VdsLoader, nullptr, CLSCTX_LOCAL_SERVER,
        IID_PPV_ARGS(&loader)), L"Create VdsServiceLoader");

    CComPtr<IVdsService> service;
    CheckHr(loader->LoadService(nullptr, &service), L"Load VDS service");
    CheckHr(service->WaitForServiceReady(), L"Wait for VDS ready");
    return service;
}

CComPtr<IVdsDisk> FindDisk(IVdsService* service, DWORD diskNumber) {
    CComPtr<IEnumVdsObject> enumProviders;
    CheckHr(service->QueryProviders(VDS_QUERY_PROVIDER_FLAG_SOFTWARE_PROVIDERS, &enumProviders),
        L"Query VDS providers");

    ULONG fetched = 0;
    CComPtr<IUnknown> unkProvider;
    while (enumProviders->Next(1, &unkProvider, &fetched) == S_OK && fetched == 1) {
        CComPtr<IVdsProvider> provider;
        CheckHr(unkProvider->QueryInterface(IID_PPV_ARGS(&provider)), L"QI IVdsProvider");

        CComPtr<IVdsSwProvider> swProvider;
        if (FAILED(provider->QueryInterface(IID_PPV_ARGS(&swProvider)))) {
            unkProvider.Release();
            continue;
        }

        CComPtr<IEnumVdsObject> enumPacks;
        CheckHr(swProvider->QueryPacks(&enumPacks), L"Query packs");

        CComPtr<IUnknown> unkPack;
        while (enumPacks->Next(1, &unkPack, &fetched) == S_OK && fetched == 1) {
            CComPtr<IVdsPack> pack;
            CheckHr(unkPack->QueryInterface(IID_PPV_ARGS(&pack)), L"QI IVdsPack");

            CComPtr<IEnumVdsObject> enumDisks;
            CheckHr(pack->QueryDisks(&enumDisks), L"Query disks");

            CComPtr<IUnknown> unkDisk;
            while (enumDisks->Next(1, &unkDisk, &fetched) == S_OK && fetched == 1) {
                CComPtr<IVdsDisk> disk;
                CheckHr(unkDisk->QueryInterface(IID_PPV_ARGS(&disk)), L"QI IVdsDisk");

                VDS_DISK_PROP prop{};
                CheckHr(disk->GetProperties(&prop), L"Get disk properties");
                if (prop.ulFlags & VDS_DF_OFFLINE) {
                    std::wcout << L"[warn] Disk " << prop.ulDiskId << L" is offline.\n";
                }
                if (prop.ulDiskId == diskNumber) {
                    return disk;
                }
                unkDisk.Release();
            }
            unkPack.Release();
        }
        unkProvider.Release();
    }

    throw std::runtime_error("Target disk not found.");
}

std::vector<CComPtr<IVdsVolumeMF>> QueryVolumeMFsByDisk(IVdsService* service, VDS_OBJECT_ID diskId) {
    std::vector<CComPtr<IVdsVolumeMF>> result;

    CComPtr<IEnumVdsObject> enumVolumes;
    CheckHr(service->QueryVolumes(&enumVolumes), L"Query volumes");

    ULONG fetched = 0;
    CComPtr<IUnknown> unkVolume;
    while (enumVolumes->Next(1, &unkVolume, &fetched) == S_OK && fetched == 1) {
        CComPtr<IVdsVolume> volume;
        CheckHr(unkVolume->QueryInterface(IID_PPV_ARGS(&volume)), L"QI IVdsVolume");

        CComPtr<IEnumVdsObject> enumPlexes;
        CheckHr(volume->QueryPlexes(&enumPlexes), L"Query plexes");

        CComPtr<IUnknown> unkPlex;
        while (enumPlexes->Next(1, &unkPlex, &fetched) == S_OK && fetched == 1) {
            CComPtr<IVdsVolumePlex> plex;
            CheckHr(unkPlex->QueryInterface(IID_PPV_ARGS(&plex)), L"QI IVdsVolumePlex");

            LONG diskCount = 0;
            VDS_DISK_EXTENT* extents = nullptr;
            CheckHr(plex->QueryExtents(&extents, &diskCount), L"Query extents");
            for (LONG i = 0; i < diskCount; ++i) {
                if (extents[i].idDisk == diskId) {
                    CComPtr<IVdsVolumeMF> volumeMf;
                    if (SUCCEEDED(volume->QueryInterface(IID_PPV_ARGS(&volumeMf)))) {
                        result.push_back(volumeMf);
                    }
                    break;
                }
            }
            CoTaskMemFree(extents);
            unkPlex.Release();
        }

        unkVolume.Release();
    }

    return result;
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

        CheckHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"CoInitializeEx");
        CheckHr(CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
            RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr), L"CoInitializeSecurity");

        CComPtr<IVdsService> service = ConnectVdsService();
        CComPtr<IVdsDisk> disk = FindDisk(service, opt.diskNumber);

        CComPtr<IVdsAdvancedDisk> advDisk;
        CheckHr(disk->QueryInterface(IID_PPV_ARGS(&advDisk)), L"QI IVdsAdvancedDisk");

        if (opt.gpt) {
            std::wcout << L"Initializing disk as GPT...\n";
            InitializeDiskGpt(advDisk);
        }

        for (const auto& req : opt.partitions) {
            CREATE_PARTITION_PARAMETERS params{};
            BuildCreateParams(req, params);

            CComPtr<IVdsAsync> createAsync;
            CheckHr(advDisk->CreatePartition(
                req.sizeBytes,
                req.offsetBytes.value_or(0),
                req.offsetBytes.has_value() ? 0 : 1024 * 1024,
                &params,
                &createAsync), L"Create GPT partition");

            WaitAsync(createAsync, L"Create partition");
            std::wcout << L"Created partition size=" << req.sizeBytes << L" bytes\n";

            if (req.format.has_value()) {
                VDS_DISK_PROP prop{};
                CheckHr(disk->GetProperties(&prop), L"Get disk props for format path");
                auto volumes = QueryVolumeMFsByDisk(service, prop.id);
                if (volumes.empty()) {
                    throw std::runtime_error("No volume found for formatting. Wait and retry may be needed.");
                }
                std::wcout << L"Formatting latest volume...\n";
                FormatVolume(volumes.back(), req.format.value());
            }
        }

        std::wcout << L"Done.\n";
        CoUninitialize();
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }
}
