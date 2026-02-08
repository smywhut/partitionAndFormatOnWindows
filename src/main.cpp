#include <Windows.h>
#include <Wbemidl.h>
#include <comdef.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "wbemuuid.lib")

namespace {

struct ComInit {
    ComInit() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            throw std::runtime_error("CoInitializeEx failed: " + std::to_string(static_cast<long>(hr)));
        }

        hr = CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr);
        if (FAILED(hr) && hr != RPC_E_TOO_LATE) {
            CoUninitialize();
            throw std::runtime_error("CoInitializeSecurity failed: " + std::to_string(static_cast<long>(hr)));
        }
    }

    ~ComInit() { CoUninitialize(); }
};

std::string HrToString(HRESULT hr) {
    _com_error err(hr);
    std::wstringstream ws;
    ws << L"HRESULT=0x" << std::hex << hr << L", " << err.ErrorMessage();
    std::wstring wide = ws.str();
    return std::string(wide.begin(), wide.end());
}

struct DiskInfo {
    uint32_t number{};
    std::wstring friendlyName;
    uint64_t size{};
    std::wstring partitionStyle;
    bool isOffline{};
};

class WmiSession {
  public:
    WmiSession() {
        HRESULT hr = locator_.CoCreateInstance(CLSID_WbemLocator);
        if (FAILED(hr)) {
            throw std::runtime_error("CoCreateInstance(CLSID_WbemLocator) failed: " + HrToString(hr));
        }

        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Storage");
        hr = locator_->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &services_);
        SysFreeString(ns);
        if (FAILED(hr)) {
            throw std::runtime_error("ConnectServer(ROOT\\Microsoft\\Windows\\Storage) failed: " + HrToString(hr));
        }

        hr = CoSetProxyBlanket(
            services_,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE);
        if (FAILED(hr)) {
            throw std::runtime_error("CoSetProxyBlanket failed: " + HrToString(hr));
        }
    }

    std::vector<DiskInfo> EnumerateDisks() {
        std::vector<DiskInfo> out;
        CComPtr<IEnumWbemClassObject> enumerator;
        BSTR wql = SysAllocString(L"WQL");
        BSTR query = SysAllocString(
            L"SELECT Number,FriendlyName,Size,PartitionStyle,IsOffline FROM MSFT_Disk");

        HRESULT hr = services_->ExecQuery(
            wql,
            query,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            &enumerator);
        SysFreeString(wql);
        SysFreeString(query);

        if (FAILED(hr)) {
            throw std::runtime_error("ExecQuery(MSFT_Disk) failed: " + HrToString(hr));
        }

        while (true) {
            CComPtr<IWbemClassObject> obj;
            ULONG ret = 0;
            hr = enumerator->Next(WBEM_INFINITE, 1, &obj, &ret);
            if (FAILED(hr)) {
                throw std::runtime_error("IEnumWbemClassObject::Next failed: " + HrToString(hr));
            }
            if (ret == 0) {
                break;
            }

            DiskInfo di;
            VARIANT v;
            VariantInit(&v);

            if (SUCCEEDED(obj->Get(L"Number", 0, &v, nullptr, nullptr)) && v.vt == VT_UI4) {
                di.number = v.ulVal;
            }
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"FriendlyName", 0, &v, nullptr, nullptr)) && v.vt == VT_BSTR) {
                di.friendlyName = v.bstrVal;
            }
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"Size", 0, &v, nullptr, nullptr)) && v.vt == VT_UI8) {
                di.size = v.ullVal;
            }
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"PartitionStyle", 0, &v, nullptr, nullptr)) && v.vt == VT_UI2) {
                switch (v.uiVal) {
                case 1:
                    di.partitionStyle = L"MBR";
                    break;
                case 2:
                    di.partitionStyle = L"GPT";
                    break;
                default:
                    di.partitionStyle = L"RAW";
                    break;
                }
            }
            VariantClear(&v);

            VariantInit(&v);
            if (SUCCEEDED(obj->Get(L"IsOffline", 0, &v, nullptr, nullptr)) && v.vt == VT_BOOL) {
                di.isOffline = (v.boolVal == VARIANT_TRUE);
            }
            VariantClear(&v);

            out.push_back(std::move(di));
        }

        return out;
    }

  private:
    CComPtr<IWbemLocator> locator_;
    CComPtr<IWbemServices> services_;
};

struct FormatConfig {
    std::string fs;  // ntfs/fat32/exfat
    std::string vol;
    bool quick = true;
};

struct PartitionConfig {
    std::optional<uint64_t> offsetBytes;
    uint64_t sizeBytes = 0;
    std::string gptPartLabel;
    std::string gptType = "{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"; // Basic Data
    std::optional<FormatConfig> format;
};

struct CliConfig {
    int diskNumber = -1;
    bool gpt = false;
    std::vector<PartitionConfig> partitions;
};

std::string Trim(std::string s) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) {
        out.push_back(item);
    }
    return out;
}

uint64_t ParseSizeBytes(const std::string& input) {
    std::string s = Trim(input);
    if (s.empty()) {
        throw std::invalid_argument("size/offset is empty");
    }

    char suffix = static_cast<char>(std::toupper(s.back()));
    uint64_t mul = 1;
    if (std::isalpha(static_cast<unsigned char>(suffix))) {
        s.pop_back();
        switch (suffix) {
        case 'K': mul = 1024ULL; break;
        case 'M': mul = 1024ULL * 1024ULL; break;
        case 'G': mul = 1024ULL * 1024ULL * 1024ULL; break;
        case 'T': mul = 1024ULL * 1024ULL * 1024ULL * 1024ULL; break;
        default: throw std::invalid_argument("unsupported size suffix: " + std::string(1, suffix));
        }
    }

    uint64_t base = 0;
    try {
        base = std::stoull(s);
    } catch (...) {
        throw std::invalid_argument("invalid number in size/offset: " + input);
    }
    return base * mul;
}

std::string NormalizeFs(const std::string& fs) {
    std::string f = fs;
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (f != "ntfs" && f != "fat32" && f != "exfat") {
        throw std::invalid_argument("fs must be ntfs/fat32/exfat");
    }
    return f;
}

std::string ResolveType(const std::string& type) {
    std::string t = type;
    std::transform(t.begin(), t.end(), t.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (t.empty() || t == "basic") {
        return "{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
    }
    return type; // assume user provides GUID.
}

void ParseKeyValues(const std::string& text, const std::function<void(const std::string&, const std::string&)>& onKv) {
    for (const auto& token : Split(text, ',')) {
        auto pos = token.find('=');
        if (pos == std::string::npos) {
            throw std::invalid_argument("invalid key=value token: " + token);
        }
        onKv(Trim(token.substr(0, pos)), Trim(token.substr(pos + 1)));
    }
}

CliConfig ParseCli(int argc, char** argv) {
    CliConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg.rfind("--disk=", 0) == 0) {
            cfg.diskNumber = std::stoi(arg.substr(7));
        } else if (arg == "--gpt") {
            cfg.gpt = true;
        } else if (arg.rfind("--create-part=", 0) == 0) {
            PartitionConfig part;
            ParseKeyValues(arg.substr(std::string("--create-part=").size()), [&](const std::string& k, const std::string& v) {
                if (k == "size") {
                    part.sizeBytes = ParseSizeBytes(v);
                } else if (k == "offset") {
                    part.offsetBytes = ParseSizeBytes(v);
                } else if (k == "label") {
                    part.gptPartLabel = v;
                } else if (k == "type") {
                    part.gptType = ResolveType(v);
                } else {
                    throw std::invalid_argument("unknown create-part key: " + k);
                }
            });
            if (part.sizeBytes == 0) {
                throw std::invalid_argument("--create-part must contain size=");
            }
            cfg.partitions.push_back(std::move(part));
        } else if (arg.rfind("--format=", 0) == 0) {
            if (cfg.partitions.empty()) {
                throw std::invalid_argument("--format must follow a --create-part");
            }
            FormatConfig fmt;
            ParseKeyValues(arg.substr(std::string("--format=").size()), [&](const std::string& k, const std::string& v) {
                if (k == "fs") {
                    fmt.fs = NormalizeFs(v);
                } else if (k == "vol") {
                    fmt.vol = v;
                } else if (k == "quick") {
                    fmt.quick = (v == "1" || v == "true" || v == "True");
                } else {
                    throw std::invalid_argument("unknown format key: " + k);
                }
            });
            if (fmt.fs.empty()) {
                throw std::invalid_argument("--format must contain fs=");
            }
            cfg.partitions.back().format = std::move(fmt);
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage:\n"
                      << "  disk_part_fmt --disk=1 --gpt \\\n"
                      << "    --create-part=size=10G,label=MyPart,type=basic \\\n"
                      << "    --format=fs=ntfs,vol=Data,quick=1\n";
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + arg);
        }
    }

    if (cfg.diskNumber < 0) {
        throw std::invalid_argument("--disk is required");
    }
    if (cfg.partitions.empty()) {
        throw std::invalid_argument("at least one --create-part is required");
    }

    return cfg;
}

std::string QuotePs(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') {
            out += "''";
        } else {
            out += c;
        }
    }
    out += "'";
    return out;
}

std::string BuildPowerShellScript(const CliConfig& cfg) {
    std::ostringstream ps;
    ps << "$ErrorActionPreference = 'Stop'\n";
    ps << "Import-Module Storage\n";
    ps << "$disk = Get-Disk -Number " << cfg.diskNumber << "\n";
    ps << "if ($disk.IsOffline) { Set-Disk -Number " << cfg.diskNumber << " -IsOffline $false }\n";
    ps << "if ($disk.IsReadOnly) { Set-Disk -Number " << cfg.diskNumber << " -IsReadOnly $false }\n";

    if (cfg.gpt) {
        ps << "Initialize-Disk -Number " << cfg.diskNumber << " -PartitionStyle GPT -ErrorAction Stop | Out-Null\n";
    }

    for (const auto& p : cfg.partitions) {
        ps << "$newPartArgs = @{ DiskNumber = " << cfg.diskNumber << "; Size = " << p.sizeBytes << "; GptType = " << QuotePs(p.gptType) << " }\n";
        if (p.offsetBytes.has_value()) {
            ps << "$newPartArgs.Offset = " << *p.offsetBytes << "\n";
        }
        ps << "$part = New-Partition @newPartArgs\n";

        if (!p.gptPartLabel.empty()) {
            ps << "try { Set-Partition -DiskNumber " << cfg.diskNumber
               << " -PartitionNumber $part.PartitionNumber -NewPartitionName " << QuotePs(p.gptPartLabel)
               << " -ErrorAction Stop | Out-Null } catch { Write-Warning 'Set-Partition -NewPartitionName not supported on this host; skip GPT PartLabel.' }\n";
        }

        if (p.format.has_value()) {
            const auto& f = *p.format;
            ps << "$fmtArgs = @{ Partition = $part; FileSystem = " << QuotePs(f.fs)
               << "; Confirm = $false; Force = $true }\n";
            if (!f.vol.empty()) {
                ps << "$fmtArgs.NewFileSystemLabel = " << QuotePs(f.vol) << "\n";
            }
            if (!f.quick) {
                ps << "$fmtArgs.Full = $true\n";
            }
            ps << "Format-Volume @fmtArgs | Out-Null\n";
        }
    }

    ps << "Write-Host 'All operations finished successfully.'\n";
    return ps.str();
}

int RunPowerShellScript(const std::string& script) {
    const auto temp = std::filesystem::temp_directory_path() / "smapi_disk_tool.ps1";
    {
        std::ofstream ofs(temp, std::ios::binary);
        ofs << script;
    }

    std::wstring cmd = L"powershell.exe -NoProfile -ExecutionPolicy Bypass -File \"" + temp.wstring() + L"\"";

    STARTUPINFOW si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);

    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        throw std::runtime_error("CreateProcessW(powershell) failed with Win32=" + std::to_string(GetLastError()));
    }

    std::cout << "[INFO] Waiting for Storage Management job to finish" << std::flush;
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 500);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait != WAIT_TIMEOUT) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            throw std::runtime_error("WaitForSingleObject failed with Win32=" + std::to_string(GetLastError()));
        }
        std::cout << "." << std::flush;
    }
    std::cout << "\n";

    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    return static_cast<int>(exitCode);
}

void PrintDisks(const std::vector<DiskInfo>& disks) {
    std::wcout << L"[INFO] Enumerated disks from ROOT\\Microsoft\\Windows\\Storage (MSFT_Disk):\n";
    for (const auto& d : disks) {
        std::wcout << L"  Disk " << d.number << L" | Name=" << d.friendlyName << L" | Size=" << d.size
                   << L" | Style=" << d.partitionStyle << L" | Offline=" << (d.isOffline ? L"Yes" : L"No")
                   << L"\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        ComInit com;

        const auto cfg = ParseCli(argc, argv);

        WmiSession wmi;
        const auto disks = wmi.EnumerateDisks();
        PrintDisks(disks);

        auto it = std::find_if(disks.begin(), disks.end(), [&](const DiskInfo& d) {
            return static_cast<int>(d.number) == cfg.diskNumber;
        });
        if (it == disks.end()) {
            throw std::runtime_error("target disk not found: " + std::to_string(cfg.diskNumber));
        }

        std::cout << "[INFO] Target disk found, start provisioning.\n";

        const auto script = BuildPowerShellScript(cfg);
        const int exitCode = RunPowerShellScript(script);
        if (exitCode != 0) {
            throw std::runtime_error("PowerShell Storage operation failed, exitCode=" + std::to_string(exitCode) +
                                     ". Common causes: access denied (run as Administrator), disk in use, or alignment/size issue.");
        }

        std::cout << "[OK] Disk partition/format finished.\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "[ERROR] " << ex.what() << "\n";
        return 1;
    }
}
