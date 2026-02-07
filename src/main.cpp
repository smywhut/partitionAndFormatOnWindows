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
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <cwctype>

#pragma comment(lib, "vdsuuid.lib")

using ATL::CComPtr;

namespace {

    constexpr GUID kBasicDataPartitionGuid =
    { 0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7} };

    // GUID 比较器，用于在 std::set 中使用
    struct GuidCompare {
        bool operator()(const GUID& a, const GUID& b) const {
            return memcmp(&a, &b, sizeof(GUID)) < 0;
        }
    };

    // 定义 GPT 分区信息结构（与 VDS 内部布局匹配）
    struct GptPartitionData {
        GUID partitionType;
        GUID partitionId;
        ULONGLONG attributes;
        WCHAR name[36];
    };

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

    std::string WideToUtf8(std::wstring_view input) {
        if (input.empty()) {
            return {};
        }

        const int size = WideCharToMultiByte(
            CP_UTF8,
            0,
            input.data(),
            static_cast<int>(input.size()),
            nullptr,
            0,
            nullptr,
            nullptr);

        if (size <= 0) {
            return "<utf8-convert-failed>";
        }

        std::string result(static_cast<size_t>(size), '\0');
        const int written = WideCharToMultiByte(
            CP_UTF8,
            0,
            input.data(),
            static_cast<int>(input.size()),
            result.data(),
            size,
            nullptr,
            nullptr);

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

    void CheckHr(HRESULT hr, const std::wstring& message) {
        if (FAILED(hr)) {
            _com_error err(hr);
            std::wstringstream ss;
            ss << message << L" (hr=0x" << std::hex << hr << L", " << err.ErrorMessage() << L")";
            throw std::runtime_error(WideToUtf8(ss.str()));
        }
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

        const double bytes = value * static_cast<double>(multiplier);
        const ULONGLONG maxValue = (std::numeric_limits<ULONGLONG>::max)();
        if (bytes > static_cast<double>(maxValue)) {
            throw std::runtime_error("Size value too large.");
        }

        return static_cast<ULONGLONG>(bytes);
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

    class ScopedCoInit {
    public:
        ScopedCoInit() {
            CheckHr(CoInitializeEx(nullptr, COINIT_MULTITHREADED), L"CoInitializeEx");
            initialized_ = true;

            CheckHr(CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                RPC_C_IMP_LEVEL_IMPERSONATE,
                nullptr,
                EOAC_NONE,
                nullptr), L"CoInitializeSecurity");
        }

        ~ScopedCoInit() {
            if (initialized_) {
                CoUninitialize();
            }
        }

        ScopedCoInit(const ScopedCoInit&) = delete;
        ScopedCoInit& operator=(const ScopedCoInit&) = delete;

    private:
        bool initialized_{ false };
    };

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
        CheckHr(advDisk->Clean(TRUE, FALSE, FALSE, &async), L"Clean disk before GPT conversion");
        WaitAsync(async, L"Clean disk");

        std::wcout << L"Disk cleaned successfully.\n";
        std::wcout << L"Note: Please use diskpart to convert to GPT manually:\n";
        std::wcout << L"  diskpart\n";
        std::wcout << L"  select disk <number>\n";
        std::wcout << L"  convert gpt\n";
    }

    void BuildCreateParams(const PartitionRequest& req, CREATE_PARTITION_PARAMETERS& params) {
        ZeroMemory(&params, sizeof(params));
        params.style = VDS_PST_GPT;

        // 使用内存偏移直接访问联合体数据
        // CREATE_PARTITION_PARAMETERS 布局: [4字节 style] [联合体数据]
        // 联合体偏移通常在 style 之后（考虑对齐）

        GptPartitionData* gptData = reinterpret_cast<GptPartitionData*>(
            reinterpret_cast<unsigned char*>(&params) + sizeof(VDS_PARTITION_STYLE));

        gptData->partitionType = req.partType;
        gptData->partitionId = GUID_NULL;
        gptData->attributes = 0;

        // 复制分区名称
        const size_t maxLen = 35;
        const size_t copyLen = (std::min)(req.partLabel.size(), maxLen);

        for (size_t i = 0; i < copyLen; ++i) {
            gptData->name[i] = req.partLabel[i];
        }
        gptData->name[copyLen] = L'\0';
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
            0,  // dwUnitAllocationSize - use default
            0,  // bForce - FALSE
            fmt.quick,
            FALSE,  // bEnableCompression
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
        CheckHr(service->QueryProviders(VDS_QUERY_SOFTWARE_PROVIDERS, &enumProviders),
            L"Query VDS providers");

        ULONG fetched = 0;
        DWORD currentDiskIndex = 0;
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

                    if (prop.status == VDS_DS_OFFLINE) {
                        std::wcout << L"[warn] Disk at index " << currentDiskIndex << L" is offline.\n";
                    }

                    if (currentDiskIndex == diskNumber) {
                        std::wcout << L"Found disk at index " << diskNumber << L"\n";
                        return disk;
                    }

                    currentDiskIndex++;
                    unkDisk.Release();
                }
                unkPack.Release();
            }
            unkProvider.Release();
        }

        throw std::runtime_error("Target disk not found.");
    }

    struct VolumeInfo {
        VDS_OBJECT_ID id{};
        CComPtr<IVdsVolumeMF> volumeMf;
    };

    std::vector<VolumeInfo> QueryVolumeMFsByDisk(IVdsService* service, VDS_OBJECT_ID diskId) {
        std::vector<VolumeInfo> result;

        CComPtr<IEnumVdsObject> enumProviders;
        CheckHr(service->QueryProviders(VDS_QUERY_SOFTWARE_PROVIDERS, &enumProviders),
            L"Query providers for volumes");

        ULONG fetched = 0;
        CComPtr<IUnknown> unkProvider;
        while (enumProviders->Next(1, &unkProvider, &fetched) == S_OK && fetched == 1) {
            CComPtr<IVdsProvider> provider;
            CheckHr(unkProvider->QueryInterface(IID_PPV_ARGS(&provider)), L"QI provider");

            CComPtr<IVdsSwProvider> swProvider;
            if (SUCCEEDED(provider->QueryInterface(IID_PPV_ARGS(&swProvider)))) {
                CComPtr<IEnumVdsObject> enumPacks;
                CheckHr(swProvider->QueryPacks(&enumPacks), L"Query packs for volumes");

                CComPtr<IUnknown> unkPack;
                while (enumPacks->Next(1, &unkPack, &fetched) == S_OK && fetched == 1) {
                    CComPtr<IVdsPack> pack;
                    CheckHr(unkPack->QueryInterface(IID_PPV_ARGS(&pack)), L"QI pack");

                    CComPtr<IEnumVdsObject> enumVolumes;
                    CheckHr(pack->QueryVolumes(&enumVolumes), L"Query volumes");

                    CComPtr<IUnknown> unkVolume;
                    while (enumVolumes->Next(1, &unkVolume, &fetched) == S_OK && fetched == 1) {
                        CComPtr<IVdsVolume> volume;
                        CheckHr(unkVolume->QueryInterface(IID_PPV_ARGS(&volume)), L"QI volume");

                        CComPtr<IEnumVdsObject> enumPlexes;
                        CheckHr(volume->QueryPlexes(&enumPlexes), L"Query plexes");

                        CComPtr<IUnknown> unkPlex;
                        while (enumPlexes->Next(1, &unkPlex, &fetched) == S_OK && fetched == 1) {
                            CComPtr<IVdsVolumePlex> plex;
                            CheckHr(unkPlex->QueryInterface(IID_PPV_ARGS(&plex)), L"QI plex");

                            LONG diskCount = 0;
                            VDS_DISK_EXTENT* extents = nullptr;
                            CheckHr(plex->QueryExtents(&extents, &diskCount), L"Query extents");
                            for (LONG i = 0; i < diskCount; ++i) {
                                if (extents[i].diskId == diskId) {
                                    CComPtr<IVdsVolumeMF> volumeMf;
                                    if (SUCCEEDED(volume->QueryInterface(IID_PPV_ARGS(&volumeMf)))) {
                                        VDS_VOLUME_PROP volumeProp{};
                                        CheckHr(volume->GetProperties(&volumeProp), L"Get volume properties");
                                        result.push_back(VolumeInfo{ volumeProp.id, volumeMf });
                                    }
                                    break;
                                }
                            }
                            CoTaskMemFree(extents);
                            unkPlex.Release();
                        }

                        unkVolume.Release();
                    }
                    unkPack.Release();
                }
            }
            unkProvider.Release();
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
        ScopedCoInit coInit;

        CComPtr<IVdsService> service = ConnectVdsService();
        CComPtr<IVdsDisk> disk = FindDisk(service, opt.diskNumber);

        CComPtr<IVdsAdvancedDisk> advDisk;
        CheckHr(disk->QueryInterface(IID_PPV_ARGS(&advDisk)), L"QI IVdsAdvancedDisk");

        if (opt.gpt) {
            std::wcout << L"Initializing disk as GPT...\n";
            InitializeDiskGpt(advDisk);
            std::wcout << L"Note: On Windows 10, you may need to manually convert to GPT using diskpart.\n";
        }

        for (const auto& req : opt.partitions) {
            VDS_DISK_PROP prop{};
            CheckHr(disk->GetProperties(&prop), L"Get disk props before partition create");
            auto volumesBefore = QueryVolumeMFsByDisk(service, prop.id);
            std::set<VDS_OBJECT_ID, GuidCompare> existingVolumeIds;
            for (const auto& volume : volumesBefore) {
                existingVolumeIds.insert(volume.id);
            }

            CREATE_PARTITION_PARAMETERS params{};
            BuildCreateParams(req, params);

            CComPtr<IVdsAsync> createAsync;
            // Windows 10 VDS: CreatePartition(offset, size, params, async)
            CheckHr(advDisk->CreatePartition(
                req.offsetBytes.value_or(0),
                req.sizeBytes,
                &params,
                &createAsync), L"Create GPT partition");

            WaitAsync(createAsync, L"Create partition");
            std::wcout << L"Created partition size=" << req.sizeBytes << L" bytes\n";

            if (req.format.has_value()) {
                // Wait for the system to recognize the new partition
                Sleep(2000);

                auto volumes = QueryVolumeMFsByDisk(service, prop.id);
                if (volumes.empty()) {
                    throw std::runtime_error("No volume found for formatting. Wait and retry may be needed.");
                }

                CComPtr<IVdsVolumeMF> targetVolume = volumes.back().volumeMf;
                for (const auto& volume : volumes) {
                    if (existingVolumeIds.find(volume.id) == existingVolumeIds.end()) {
                        targetVolume = volume.volumeMf;
                        break;
                    }
                }

                std::wcout << L"Formatting latest volume...\n";
                FormatVolume(targetVolume, req.format.value());
            }
        }

        std::wcout << L"Done.\n";
        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << "\n";
        return 2;
    }
}