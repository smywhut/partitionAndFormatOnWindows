/*
 * Windows Storage Management API 磁盘分区与格式化工具 (ATL 版本)
 *
 * 编译要求:
 *   - Windows 10+ SDK
 *   - C++17 或更高版本
 *   - ATL 库支持
 *   - 链接库: ole32.lib oleaut32.lib wbemuuid.lib
 *
 * 编译命令:
 *   cl /EHsc /std:c++17 DiskPartitionTool_ATL.cpp /link ole32.lib oleaut32.lib wbemuuid.lib
 *
 * 使用示例:
 *   DiskPartitionTool.exe --disk=1 --gpt --create-part size=10G,label=MyPart --format fs=ntfs,vol=Data,quick=1
 */

#include <windows.h>
#include <atlbase.h>      // ATL 基础类
#include <atlcom.h>       // ATL COM 支持
#include <comdef.h>
#include <Wbemidl.h>
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <iomanip>
#include <memory>
#include <thread>
#include <chrono>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <atlbase.h>
#include <atlcomcli.h>
#include <functional>
#include <string>
#include <vector>
#include <map>

#include <io.h>
#include <fcntl.h>

#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

using namespace std;
using namespace ATL;

// ================================
// 常量定义
// ================================

// GPT 分区类型 GUID
const wstring GUID_BASIC_DATA_PARTITION = L"{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}";
const wstring GUID_EFI_SYSTEM_PARTITION = L"{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}";
const wstring GUID_MICROSOFT_RESERVED = L"{E3C9E316-0B5C-4DB8-817D-F92DF00215AE}";

// 文件系统类型
const map<wstring, int> FILE_SYSTEMS = {
    {L"ntfs", 7},    // NTFS
    {L"fat32", 5},   // FAT32
    {L"exfat", 8},   // exFAT
    {L"refs", 9}     // ReFS
};

// ================================
// 工具函数
// ================================

// 转换字节大小的字符串(如 "10G", "500M") 为字节数
ULONGLONG ParseSizeString(const wstring& sizeStr) {
    wstring str = sizeStr;
    ULONGLONG multiplier = 1;

    if (!str.empty()) {
        wchar_t unit = towupper(str.back());
        if (unit == L'K' || unit == L'M' || unit == L'G' || unit == L'T') {
            str.pop_back();
            switch (unit) {
            case L'K': multiplier = 1024ULL; break;
            case L'M': multiplier = 1024ULL * 1024; break;
            case L'G': multiplier = 1024ULL * 1024 * 1024; break;
            case L'T': multiplier = 1024ULL * 1024 * 1024 * 1024; break;
            }
        }
    }

    return stoull(str) * multiplier;
}

// 解析参数字符串 (如 "size=10G,label=MyPart,type=basic")
map<wstring, wstring> ParseParams(const wstring& paramStr) {
    map<wstring, wstring> params;
    wstringstream ss(paramStr);
    wstring token;

    while (getline(ss, token, L',')) {
        size_t pos = token.find(L'=');
        if (pos != wstring::npos) {
            wstring key = token.substr(0, pos);
            wstring value = token.substr(pos + 1);
            params[key] = value;
        }
    }

    return params;
}

// GUID 字符串转换
wstring PartitionTypeToGuid(const wstring& type) {
    if (type == L"basic" || type == L"data") {
        return GUID_BASIC_DATA_PARTITION;
    }
    else if (type == L"efi") {
        return GUID_EFI_SYSTEM_PARTITION;
    }
    else if (type == L"msr") {
        return GUID_MICROSOFT_RESERVED;
    }
    return type; // 假设是完整 GUID
}

// ================================
// WMI 管理类 (ATL 版本)
// ================================

class WMIManager {
private:
    CComPtr<IWbemLocator> pLoc;      // ATL 智能指针，自动管理引用计数
    CComPtr<IWbemServices> pSvc;     // ATL 智能指针
    bool initialized;

public:
    WMIManager() : initialized(false) {}

    ~WMIManager() {
        Cleanup();
    }

    bool Initialize() {
        HRESULT hres;

        // 初始化 COM
        hres = CoInitializeEx(0, COINIT_MULTITHREADED);
        if (FAILED(hres) && hres != RPC_E_CHANGED_MODE) {
            wcerr << L"❌ COM 初始化失败. 错误代码: 0x" << hex << hres << endl;
            return false;
        }

        // 设置 COM 安全级别
        hres = CoInitializeSecurity(
            NULL,
            -1,
            NULL,
            NULL,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL,
            EOAC_NONE,
            NULL
        );

        if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
            wcerr << L"❌ COM 安全初始化失败. 错误代码: 0x" << hex << hres << endl;
            CoUninitialize();
            return false;
        }

        // 获取 WMI 定位器 - 使用 ATL 智能指针
        hres = pLoc.CoCreateInstance(CLSID_WbemLocator);
        if (FAILED(hres)) {
            wcerr << L"❌ 创建 WbemLocator 失败. 错误代码: 0x" << hex << hres << endl;
            CoUninitialize();
            return false;
        }

        // 连接到 ROOT\Microsoft\Windows\Storage 命名空间
        CComBSTR bstrNamespace(L"ROOT\\Microsoft\\Windows\\Storage");
        hres = pLoc->ConnectServer(
            bstrNamespace,
            NULL,
            NULL,
            0,
            NULL,
            0,
            0,
            &pSvc
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 连接到 WMI Storage 命名空间失败. 错误代码: 0x" << hex << hres << endl;
            pLoc.Release();  // ATL 智能指针会自动 Release，但可以显式调用
            CoUninitialize();
            return false;
        }

        // 设置代理安全级别
        hres = CoSetProxyBlanket(
            pSvc,
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            NULL,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            NULL,
            EOAC_NONE
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 设置代理安全失败. 错误代码: 0x" << hex << hres << endl;
            pSvc.Release();
            pLoc.Release();
            CoUninitialize();
            return false;
        }

        initialized = true;
        wcout << L"✓ WMI 连接成功" << endl;
        return true;
    }

    void Cleanup() {
        // ATL 智能指针会自动释放，但确保 COM 清理
        pSvc.Release();
        pLoc.Release();
        if (initialized) {
            CoUninitialize();
            initialized = false;
        }
    }

    IWbemServices* GetServices() { return pSvc; }

    // 执行 WMI 方法
    bool ExecMethod(
        const wstring& objectPath,
        const wstring& methodName,
        IWbemClassObject* pInParams,
        CComPtr<IWbemClassObject>& pOutParams  // 使用 ATL 智能指针引用
    ) {
        CComBSTR bstrObjectPath(objectPath.c_str());
        CComBSTR bstrMethodName(methodName.c_str());

        HRESULT hres = pSvc->ExecMethod(
            bstrObjectPath,
            bstrMethodName,
            0,
            NULL,
            pInParams,
            &pOutParams,
            NULL
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 执行方法 " << methodName << L" 失败. 错误代码: 0x" << hex << hres << endl;
            return false;
        }

        // 检查返回值 - 使用 ATL CComVariant
        if (pOutParams) {
            CComVariant varReturnValue;

            hres = pOutParams->Get(CComBSTR(L"ReturnValue"), 0, &varReturnValue, 0, 0);
            if (SUCCEEDED(hres)) {
                LONG retVal = V_I4(&varReturnValue);
                // CComVariant 析构时自动调用 VariantClear

                if (retVal != 0) {
                    wcerr << L"❌ 方法 " << methodName << L" 返回错误码: " << retVal << endl;
                    return false;
                }
            }
        }

        return true;
    }

    // 查询对象 - 返回 ATL 智能指针
    CComPtr<IEnumWbemClassObject> Query(const wstring& query) {
        CComPtr<IEnumWbemClassObject> pEnumerator;
        CComBSTR bstrQuery(query.c_str());

        HRESULT hres = pSvc->ExecQuery(
            CComBSTR(L"WQL"),
            bstrQuery,
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            NULL,
            &pEnumerator
        );

        if (FAILED(hres)) {
            wcerr << L"❌ WMI 查询失败: " << query << endl;
            return nullptr;
        }

        return pEnumerator;
    }
};

// ================================
// 磁盘管理类 (ATL 版本)
// ================================

class DiskManager {
private:
    WMIManager& wmi;

public:
    DiskManager(WMIManager& wmiMgr) : wmi(wmiMgr) {}

    // 枚举所有物理磁盘
    void EnumerateDisks() {
        wcout << L"\n📀 枚举系统磁盘..." << endl;
        wcout << L"==========================================\n" << endl;

        auto pEnumerator = wmi.Query(L"SELECT * FROM MSFT_Disk");
        if (!pEnumerator) return;

        CComPtr<IWbemClassObject> pclsObj;
        ULONG uReturn = 0;

        while (pEnumerator) {
            HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn);
            if (uReturn == 0) break;

            // 使用 ATL CComVariant 简化 VARIANT 操作
            CComVariant vtNumber, vtSize, vtModel, vtPartitionStyle, vtIsOffline;

            pclsObj->Get(CComBSTR(L"Number"), 0, &vtNumber, 0, 0);
            pclsObj->Get(CComBSTR(L"Size"), 0, &vtSize, 0, 0);
            pclsObj->Get(CComBSTR(L"Model"), 0, &vtModel, 0, 0);
            pclsObj->Get(CComBSTR(L"PartitionStyle"), 0, &vtPartitionStyle, 0, 0);
            pclsObj->Get(CComBSTR(L"IsOffline"), 0, &vtIsOffline, 0, 0);

            wcout << L"磁盘 " << V_I4(&vtNumber) << L": ";
            if (vtModel.vt == VT_BSTR) {
                wcout << V_BSTR(&vtModel);
            }
            wcout << endl;

            wcout << L"  大小: " << fixed << setprecision(2)
                << (V_UI8(&vtSize) / (1024.0 * 1024.0 * 1024.0)) << L" GB" << endl;

            wcout << L"  分区样式: ";
            switch (V_I4(&vtPartitionStyle)) {
            case 0: wcout << L"RAW (未初始化)"; break;
            case 1: wcout << L"MBR"; break;
            case 2: wcout << L"GPT"; break;
            default: wcout << L"未知"; break;
            }
            wcout << endl;

            wcout << L"  状态: " << (V_BOOL(&vtIsOffline) ? L"离线" : L"在线") << endl;
            wcout << endl;

            // CComVariant 和 CComPtr 会自动清理
            pclsObj.Release();  // 准备下一次循环
        }
    }

    // 初始化磁盘为 GPT
    bool InitializeAsGPT(int diskNumber) {
        wcout << L"\n🔧 初始化磁盘 " << diskNumber << L" 为 GPT..." << endl;

        wstring diskPath = L"\\\\.\\ROOT\\Microsoft\\Windows\\Storage:MSFT_Disk.Number=" + to_wstring(diskNumber);

        // 获取 MSFT_Disk 类
        CComPtr<IWbemClassObject> pClass;
        CComBSTR bstrClassName(L"MSFT_Disk");

        HRESULT hres = wmi.GetServices()->GetObject(
            bstrClassName,
            0,
            NULL,
            &pClass,
            NULL
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 获取 MSFT_Disk 类失败" << endl;
            return false;
        }

        // 获取 Initialize 方法
        CComPtr<IWbemClassObject> pInParamsDefinition;
        hres = pClass->GetMethod(CComBSTR(L"Initialize"), 0, &pInParamsDefinition, NULL);
        if (FAILED(hres)) {
            wcerr << L"❌ 获取 Initialize 方法失败" << endl;
            return false;
        }

        // 创建方法参数实例
        CComPtr<IWbemClassObject> pInParams;
        pInParamsDefinition->SpawnInstance(0, &pInParams);

        // 设置分区样式为 GPT (2) - 使用 ATL CComVariant
        CComVariant varPartitionStyle(2L);  // 直接构造 LONG 类型的 VARIANT
        pInParams->Put(CComBSTR(L"PartitionStyle"), 0, &varPartitionStyle, 0);
        // CComVariant 析构时自动 VariantClear

        // 执行方法
        CComPtr<IWbemClassObject> pOutParams;
        bool result = wmi.ExecMethod(diskPath, L"Initialize", pInParams, pOutParams);

        if (result) {
            wcout << L"✓ 磁盘初始化为 GPT 成功" << endl;
        }

        return result;
    }

    // 创建 GPT 分区
    bool CreatePartition(
        int diskNumber,
        ULONGLONG size,
        const wstring& gptLabel,
        const wstring& gptType,
        ULONGLONG offset = 0
    ) {
        wcout << L"\n📝 在磁盘 " << diskNumber << L" 上创建分区..." << endl;
        wcout << L"  大小: " << (size / (1024.0 * 1024.0 * 1024.0)) << L" GB" << endl;
        wcout << L"  GPT 标签: " << gptLabel << endl;

        wstring diskPath = L"\\\\.\\ROOT\\Microsoft\\Windows\\Storage:MSFT_Disk.Number=" + to_wstring(diskNumber);

        // 获取 MSFT_Disk 类
        CComPtr<IWbemClassObject> pClass;
        HRESULT hres = wmi.GetServices()->GetObject(
            CComBSTR(L"MSFT_Disk"),
            0,
            NULL,
            &pClass,
            NULL
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 获取 MSFT_Disk 类失败" << endl;
            return false;
        }

        // 获取 CreatePartition 方法
        CComPtr<IWbemClassObject> pInParamsDefinition;
        hres = pClass->GetMethod(CComBSTR(L"CreatePartition"), 0, &pInParamsDefinition, NULL);
        if (FAILED(hres)) {
            wcerr << L"❌ 获取 CreatePartition 方法失败" << endl;
            return false;
        }

        // 创建方法参数实例
        CComPtr<IWbemClassObject> pInParams;
        pInParamsDefinition->SpawnInstance(0, &pInParams);

        // 设置参数 - 使用 ATL CComVariant，简化 VARIANT 操作

        // Size (UINT64)
        CComVariant varSize((__int64)size);
        varSize.vt = VT_UI8;  // 确保类型为 UINT64
        varSize.ullVal = size;
        pInParams->Put(CComBSTR(L"Size"), 0, &varSize, 0);

        // Offset (如果指定)
        if (offset > 0) {
            CComVariant varOffset((__int64)offset);
            varOffset.vt = VT_UI8;
            varOffset.ullVal = offset;
            pInParams->Put(CComBSTR(L"Offset"), 0, &varOffset, 0);
        }

        // GptType (分区类型 GUID) - 使用 CComBSTR
        wstring guid = PartitionTypeToGuid(gptType);
        CComBSTR bstrGuid(guid.c_str());
        CComVariant varGuid(bstrGuid);
        pInParams->Put(CComBSTR(L"GptType"), 0, &varGuid, 0);

        // UseMaximumSize = false
        CComVariant varUseMaxSize(false);
        pInParams->Put(CComBSTR(L"UseMaximumSize"), 0, &varUseMaxSize, 0);

        // 执行方法
        CComPtr<IWbemClassObject> pOutParams;
        bool result = wmi.ExecMethod(diskPath, L"CreatePartition", pInParams, pOutParams);

        // 获取创建的分区对象
        wstring partitionPath;
        if (result && pOutParams) {
            CComVariant varPartition;
            hres = pOutParams->Get(CComBSTR(L"CreatedPartition"), 0, &varPartition, 0, 0);

            if (SUCCEEDED(hres) && varPartition.vt == VT_UNKNOWN) {
                CComPtr<IWbemClassObject> pPartition;
                varPartition.punkVal->QueryInterface(IID_IWbemClassObject, (void**)&pPartition);

                if (pPartition) {
                    CComVariant varPath;
                    pPartition->Get(CComBSTR(L"__PATH"), 0, &varPath, 0, 0);
                    if (varPath.vt == VT_BSTR) {
                        partitionPath = V_BSTR(&varPath);
                        wcout << L"✓ 分区创建成功" << endl;

                        // 设置 GPT 分区标签
                        if (!gptLabel.empty()) {
                            SetGptPartitionName(partitionPath, gptLabel);
                        }
                    }
                }
            }
        }

        return result;
    }

    // 设置 GPT 分区名称
    bool SetGptPartitionName(const wstring& partitionPath, const wstring& gptLabel) {
        wcout << L"  设置 GPT 分区名称: " << gptLabel << endl;

        // 获取分区对象
        CComPtr<IWbemClassObject> pPartition;
        CComBSTR bstrPartitionPath(partitionPath.c_str());

        HRESULT hres = wmi.GetServices()->GetObject(
            bstrPartitionPath,
            0,
            NULL,
            &pPartition,
            NULL
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 获取分区对象失败" << endl;
            return false;
        }

        // 设置 GptPartitionName 属性 - 使用 CComBSTR 和 CComVariant
        CComBSTR bstrLabel(gptLabel.c_str());
        CComVariant varLabel(bstrLabel);

        hres = pPartition->Put(CComBSTR(L"GptPartitionName"), 0, &varLabel, 0);

        if (FAILED(hres)) {
            wcerr << L"❌ 设置 GptPartitionName 属性失败" << endl;
            return false;
        }

        // 提交更改
        hres = wmi.GetServices()->PutInstance(pPartition, WBEM_FLAG_UPDATE_ONLY, NULL, NULL);

        if (FAILED(hres)) {
            wcerr << L"❌ 提交 GPT 分区名称失败. 错误代码: 0x" << hex << hres << endl;
            return false;
        }

        wcout << L"  ✓ GPT 分区名称设置成功" << endl;
        return true;
    }

    // 格式化分区
    bool FormatPartition(
        int diskNumber,
        int partitionNumber,
        const wstring& fileSystem,
        const wstring& volumeLabel,
        bool quickFormat
    ) {
        wcout << L"\n💾 格式化分区 (磁盘 " << diskNumber
            << L", 分区 " << partitionNumber << L")..." << endl;
        wcout << L"  文件系统: " << fileSystem << endl;
        wcout << L"  卷标: " << volumeLabel << endl;
        wcout << L"  快速格式化: " << (quickFormat ? L"是" : L"否") << endl;

        // 构建查询获取分区
        wstringstream query;
        query << L"SELECT * FROM MSFT_Partition WHERE DiskNumber = " << diskNumber
            << L" AND PartitionNumber = " << partitionNumber;

        auto pEnumerator = wmi.Query(query.str());
        if (!pEnumerator) return false;

        CComPtr<IWbemClassObject> pPartition;
        ULONG uReturn = 0;
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pPartition, &uReturn);

        if (uReturn == 0) {
            wcerr << L"❌ 未找到指定分区" << endl;
            return false;
        }

        // 获取分区路径 - 使用 CComVariant
        CComVariant varPath;
        pPartition->Get(CComBSTR(L"__PATH"), 0, &varPath, 0, 0);
        wstring partitionPath = V_BSTR(&varPath);

        // 获取 MSFT_Partition 类
        CComPtr<IWbemClassObject> pClass;
        HRESULT hres = wmi.GetServices()->GetObject(
            CComBSTR(L"MSFT_Partition"),
            0,
            NULL,
            &pClass,
            NULL
        );

        if (FAILED(hres)) {
            wcerr << L"❌ 获取 MSFT_Partition 类失败" << endl;
            return false;
        }

        // 获取 Format 方法 (Windows 10+)
        CComPtr<IWbemClassObject> pInParamsDefinition;
        hres = pClass->GetMethod(CComBSTR(L"Format"), 0, &pInParamsDefinition, NULL);

        if (FAILED(hres)) {
            wcerr << L"❌ 获取 Format 方法失败" << endl;
            return false;
        }

        // 创建方法参数实例
        CComPtr<IWbemClassObject> pInParams;
        pInParamsDefinition->SpawnInstance(0, &pInParams);

        // FileSystem (NTFS=7, FAT32=5, exFAT=8, ReFS=9)
        auto fsIt = FILE_SYSTEMS.find(fileSystem);
        if (fsIt != FILE_SYSTEMS.end()) {
            CComVariant varFS(fsIt->second);
            pInParams->Put(CComBSTR(L"FileSystem"), 0, &varFS, 0);
        }

        // FileSystemLabel - 使用 CComBSTR
        if (!volumeLabel.empty()) {
            CComBSTR bstrLabel(volumeLabel.c_str());
            CComVariant varLabel(bstrLabel);
            pInParams->Put(CComBSTR(L"FileSystemLabel"), 0, &varLabel, 0);
        }

        // Full (快速格式化 = false, 完全格式化 = true)
        CComVariant varFull(!quickFormat);
        pInParams->Put(CComBSTR(L"Full"), 0, &varFull, 0);

        // 执行格式化
        wcout << L"  正在格式化，请稍候..." << endl;

        CComPtr<IWbemClassObject> pOutParams;
        bool result = wmi.ExecMethod(partitionPath, L"Format", pInParams, pOutParams);

        if (result) {
            wcout << L"✓ 分区格式化成功" << endl;

            // 等待格式化完成并分配盘符
            this_thread::sleep_for(chrono::seconds(2));

            // 查询新的盘符
            GetPartitionDriveLetter(diskNumber, partitionNumber);
        }

        return result;
    }

    // 获取分区盘符
    void GetPartitionDriveLetter(int diskNumber, int partitionNumber) {
        wstringstream query;
        query << L"SELECT * FROM MSFT_Partition WHERE DiskNumber = " << diskNumber
            << L" AND PartitionNumber = " << partitionNumber;

        auto pEnumerator = wmi.Query(query.str());
        if (!pEnumerator) return;

        CComPtr<IWbemClassObject> pPartition;
        ULONG uReturn = 0;
        HRESULT hr = pEnumerator->Next(WBEM_INFINITE, 1, &pPartition, &uReturn);

        if (uReturn == 0) return;

        // 使用 CComVariant
        CComVariant varLetter;
        pPartition->Get(CComBSTR(L"DriveLetter"), 0, &varLetter, 0, 0);

        if (varLetter.vt == VT_I2 && V_I2(&varLetter) != 0) {
            wcout << L"  分配盘符: " << (wchar_t)V_I2(&varLetter) << L":\\" << endl;
        }
    }
};

// ================================
// 命令行参数解析
// ================================

struct CommandLineArgs {
    int diskNumber = -1;
    bool initGpt = false;

    struct PartitionSpec {
        ULONGLONG size = 0;
        ULONGLONG offset = 0;
        wstring label;
        wstring type = L"basic";
    };
    vector<PartitionSpec> partitions;

    struct FormatSpec {
        wstring fileSystem = L"ntfs";
        wstring volumeLabel;
        bool quickFormat = true;
    };
    vector<FormatSpec> formats;

    bool listDisks = false;
};

CommandLineArgs ParseCommandLine(int argc, wchar_t* argv[]) {
    CommandLineArgs args;

    for (int i = 1; i < argc; i++) {
        wstring arg = argv[i];

        if (arg.find(L"--disk=") == 0) {
            args.diskNumber = stoi(arg.substr(7));
        }
        else if (arg == L"--gpt") {
            args.initGpt = true;
        }
        else if (arg == L"--list") {
            args.listDisks = true;
        }
        else if (arg.find(L"--create-part") == 0) {
            size_t pos = arg.find(L'=');
            if (pos != wstring::npos) {
                wstring params = arg.substr(pos + 1);
                auto paramMap = ParseParams(params);

                CommandLineArgs::PartitionSpec spec;

                if (paramMap.count(L"size")) {
                    spec.size = ParseSizeString(paramMap[L"size"]);
                }
                if (paramMap.count(L"offset")) {
                    spec.offset = ParseSizeString(paramMap[L"offset"]);
                }
                if (paramMap.count(L"label")) {
                    spec.label = paramMap[L"label"];
                }
                if (paramMap.count(L"type")) {
                    spec.type = paramMap[L"type"];
                }

                args.partitions.push_back(spec);
            }
        }
        else if (arg.find(L"--format") == 0) {
            size_t pos = arg.find(L'=');
            if (pos != wstring::npos) {
                wstring params = arg.substr(pos + 1);
                auto paramMap = ParseParams(params);

                CommandLineArgs::FormatSpec spec;

                if (paramMap.count(L"fs")) {
                    spec.fileSystem = paramMap[L"fs"];
                }
                if (paramMap.count(L"vol")) {
                    spec.volumeLabel = paramMap[L"vol"];
                }
                if (paramMap.count(L"quick")) {
                    spec.quickFormat = (paramMap[L"quick"] == L"1" || paramMap[L"quick"] == L"true");
                }

                args.formats.push_back(spec);
            }
        }
    }

    return args;
}

// ================================
// 主程序
// ================================

void PrintUsage() {
    wcout << L"\n磁盘分区与格式化工具 - Windows Storage Management API (ATL 版本)\n" << endl;
    wcout << L"用法:" << endl;
    wcout << L"  DiskPartitionTool.exe [选项]\n" << endl;
    wcout << L"选项:" << endl;
    wcout << L"  --list                          列出所有磁盘" << endl;
    wcout << L"  --disk=<N>                      指定磁盘编号" << endl;
    wcout << L"  --gpt                           初始化为 GPT 分区表" << endl;
    wcout << L"  --create-part <参数>            创建分区" << endl;
    wcout << L"      参数: size=<大小>,label=<标签>,type=<类型>,offset=<偏移>" << endl;
    wcout << L"      大小支持: 10G, 500M, 1T 等" << endl;
    wcout << L"      类型: basic, efi, msr 或完整 GUID" << endl;
    wcout << L"  --format <参数>                 格式化分区" << endl;
    wcout << L"      参数: fs=<文件系统>,vol=<卷标>,quick=<0|1>" << endl;
    wcout << L"      文件系统: ntfs, fat32, exfat, refs\n" << endl;
    wcout << L"示例:" << endl;
    wcout << L"  列出磁盘:" << endl;
    wcout << L"    DiskPartitionTool.exe --list\n" << endl;
    wcout << L"  完整操作示例:" << endl;
    wcout << L"    DiskPartitionTool.exe --disk=1 --gpt \\" << endl;
    wcout << L"      --create-part size=100M,label=EFI,type=efi \\" << endl;
    wcout << L"      --format fs=fat32,quick=1 \\" << endl;
    wcout << L"      --create-part size=50G,label=Windows,type=basic \\" << endl;
    wcout << L"      --format fs=ntfs,vol=System,quick=1\n" << endl;
    wcout << L"⚠️  警告: 此工具会清除磁盘数据，请谨慎使用!" << endl;
}

int wmain(int argc, wchar_t* argv[]) {
    // 设置控制台输出为 UTF-16
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stderr), _O_U16TEXT);

    wcout << L"╔════════════════════════════════════════════════════════╗" << endl;
    wcout << L"║   Windows Storage Management API 磁盘工具 (ATL版)     ║" << endl;
    wcout << L"║   版本: 2.0 | 需要管理员权限                          ║" << endl;
    wcout << L"╚════════════════════════════════════════════════════════╝\n" << endl;

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    // 检查管理员权限
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }

    if (!isAdmin) {
        wcerr << L"❌ 错误: 需要管理员权限运行此程序!" << endl;
        wcerr << L"   请右键选择 '以管理员身份运行'" << endl;
        return 1;
    }

    // 解析命令行
    auto args = ParseCommandLine(argc, argv);

    // 初始化 WMI
    WMIManager wmi;
    if (!wmi.Initialize()) {
        wcerr << L"❌ WMI 初始化失败" << endl;
        return 1;
    }

    DiskManager diskMgr(wmi);

    // 列出磁盘
    if (args.listDisks) {
        diskMgr.EnumerateDisks();
        return 0;
    }

    // 验证磁盘编号
    if (args.diskNumber < 0) {
        wcerr << L"❌ 错误: 必须指定磁盘编号 (--disk=N)" << endl;
        PrintUsage();
        return 1;
    }

    wcout << L"目标磁盘: " << args.diskNumber << endl;
    wcout << L"⚠️  警告: 此操作将清除磁盘上的所有数据!" << endl;
    wcout << L"按 'Y' 继续, 其他键取消: ";

    wchar_t confirm;
    wcin >> confirm;

    if (towupper(confirm) != L'Y') {
        wcout << L"操作已取消" << endl;
        return 0;
    }

    // 初始化为 GPT
    if (args.initGpt) {
        if (!diskMgr.InitializeAsGPT(args.diskNumber)) {
            wcerr << L"❌ GPT 初始化失败" << endl;
            return 1;
        }
    }

    // 创建分区并格式化
    int partitionNumber = 1; // GPT 分区从 1 开始

    for (size_t i = 0; i < args.partitions.size(); i++) {
        auto& partSpec = args.partitions[i];

        // 创建分区
        if (!diskMgr.CreatePartition(
            args.diskNumber,
            partSpec.size,
            partSpec.label,
            partSpec.type,
            partSpec.offset
        )) {
            wcerr << L"❌ 分区创建失败" << endl;
            return 1;
        }

        // 如果有对应的格式化参数
        if (i < args.formats.size()) {
            auto& fmtSpec = args.formats[i];

            // 等待分区就绪
            this_thread::sleep_for(chrono::seconds(1));

            if (!diskMgr.FormatPartition(
                args.diskNumber,
                partitionNumber,
                fmtSpec.fileSystem,
                fmtSpec.volumeLabel,
                fmtSpec.quickFormat
            )) {
                wcerr << L"❌ 分区格式化失败" << endl;
                return 1;
            }
        }

        partitionNumber++;
    }

    wcout << L"\n✓ 所有操作完成!" << endl;

    // ATL 智能指针会自动清理，WMIManager 析构函数会调用 Cleanup()
    return 0;
}