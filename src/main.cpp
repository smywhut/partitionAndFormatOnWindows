#include <windows.h>
#include <mi.h>
#include <iostream>
#include <string>
#include <thread>
#include <chrono>

#pragma comment(lib, "mi.lib")

using namespace std;

class StorageManager {
private:
    MI_Application app;
    MI_Session session;
    bool connected = false;
    const wchar_t* STORAGE_NAMESPACE = L"ROOT\\Microsoft\\Windows\\Storage";

    // 关键：检查 WMI 方法的逻辑返回值
    void CheckMethodOutput(const MI_Instance* out, const char* methodName) {
        if (!out) return;
        MI_Value val;
        if (MI_Instance_GetElement(out, MI_T("ReturnValue"), &val, nullptr, nullptr, nullptr) == MI_RESULT_OK) {
            unsigned int ret = val.uint32;
            if (ret == 0 || ret == 4096) { // 0: 成功, 4096: Job 已启动
                return;
            }
            cerr << "[Logic Error] " << methodName << " failed with ReturnValue: " << ret << endl;
            if (ret == 40001) cerr << "xxx" << endl;
            if (ret == 46000) cerr << "nono" << endl;
            throw runtime_error("Method execution failed");
        }
    }

    void WaitForJob(const MI_Instance* jobRef) {
        if (!jobRef) return;
        MI_Value idVal;
        MI_Instance_GetElement(jobRef, MI_T("InstanceID"), &idVal, nullptr, nullptr, nullptr);
        wcout << L"    [Job " << (idVal.string ? idVal.string : L"Task") << L"] ";

        while (true) {
            MI_Operation op = MI_OPERATION_NULL;
            MI_Session_GetInstance(&session, 0, nullptr, STORAGE_NAMESPACE, jobRef, nullptr, &op);

            const MI_Instance* jobInst = nullptr;
            MI_Boolean more;
            if (MI_Operation_GetInstance(&op, &jobInst, &more, nullptr, nullptr, nullptr) == MI_RESULT_OK && jobInst) {
                MI_Value state, percent;
                MI_Instance_GetElement(jobInst, MI_T("JobState"), &state, nullptr, nullptr, nullptr);
                MI_Instance_GetElement(jobInst, MI_T("PercentComplete"), &percent, nullptr, nullptr, nullptr);

                wcout << L"\r    [Progress] " << percent.uint16 << L"%" << flush;

                if (state.uint16 == 7) { // 7 = Completed
                    cout << " [Done]" << endl;
                    MI_Operation_Close(&op);
                    break;
                }
                if (state.uint16 > 7) {
                    MI_Operation_Close(&op);
                    throw runtime_error("Async Job failed");
                }
            }
            MI_Operation_Close(&op);
            this_thread::sleep_for(chrono::milliseconds(500));
        }
    }

public:
    StorageManager() { memset(&app, 0, sizeof(app)); memset(&session, 0, sizeof(session)); }
    ~StorageManager() { if (connected) { MI_Session_Close(&session, nullptr, nullptr); MI_Application_Close(&app); } }

    void Connect() {
        if (MI_Application_InitializeV1(0, nullptr, nullptr, &app) != MI_RESULT_OK) throw runtime_error("MI Init Fail");
        if (MI_Application_NewSession(&app, nullptr, nullptr, nullptr, nullptr, nullptr, &session) != MI_RESULT_OK) throw runtime_error("Session Fail");
        connected = true;
    }

    MI_Instance* GetDisk(int diskNumber) {
        MI_Operation op = MI_OPERATION_NULL;
        wstring query = L"SELECT * FROM MSFT_Disk WHERE Number = " + to_wstring(diskNumber);
        MI_Session_QueryInstances(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("WQL"), query.c_str(), nullptr, &op);

        const MI_Instance* inst = nullptr;
        MI_Boolean more;
        if (MI_Operation_GetInstance(&op, &inst, &more, nullptr, nullptr, nullptr) != MI_RESULT_OK || !inst) throw runtime_error("Disk not found");

        MI_Instance* cloned;
        MI_Instance_Clone(inst, &cloned);
        MI_Operation_Close(&op);
        return cloned;
    }

    // 新增：清除磁盘所有分区 (相当于 clean)
    void ClearDisk(MI_Instance* disk) {
        cout << "Step 1: Cleaning Disk (Removing all data)..." << endl;
        MI_Operation op = MI_OPERATION_NULL;
        // 注意：Clear 方法不需要 inputParams
        MI_Session_Invoke(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("MSFT_Disk"), MI_T("Clear"), disk, nullptr, nullptr, &op);

        const MI_Instance* out = nullptr;
        MI_Boolean more;
        MI_Operation_GetInstance(&op, &out, &more, nullptr, nullptr, nullptr);
        CheckMethodOutput(out, "Clear");

        MI_Value job;
        if (MI_Instance_GetElement(out, MI_T("CreatedStorageJob"), &job, nullptr, nullptr, nullptr) == MI_RESULT_OK && job.instance)
            WaitForJob(job.instance);
        MI_Operation_Close(&op);
    }

    MI_Instance* CreatePartition(MI_Instance* disk, ULONGLONG sizeBytes) {
        cout << "Step 2: Creating Partition..." << endl;
        MI_Operation classOp = MI_OPERATION_NULL;
        const MI_Class* diskClass = nullptr;
        MI_Boolean more;
        MI_Session_GetClass(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("MSFT_Disk"), nullptr, &classOp);
        MI_Operation_GetClass(&classOp, &diskClass, &more, nullptr, nullptr, nullptr);

        MI_Instance* params = nullptr;
        MI_Application_NewInstanceFromClass(&app, MI_T("MSFT_Disk"), diskClass, &params);
        MI_Value val;
        val.uint64 = sizeBytes;
        MI_Instance_SetElement(params, MI_T("Size"), &val, MI_UINT64, 0);
        val.boolean = MI_TRUE; // AssignDriveLetter
        MI_Instance_SetElement(params, MI_T("UseMaximumSize"), &val, MI_BOOLEAN, 0); // 使用全部空间则设为 true

        MI_Operation op = MI_OPERATION_NULL;
        MI_Session_Invoke(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("MSFT_Disk"), MI_T("CreatePartition"), disk, params, nullptr, &op);

        const MI_Instance* out = nullptr;
        MI_Operation_GetInstance(&op, &out, &more, nullptr, nullptr, nullptr);
        CheckMethodOutput(out, "CreatePartition");

        MI_Value job;
        if (MI_Instance_GetElement(out, MI_T("CreatedStorageJob"), &job, nullptr, nullptr, nullptr) == MI_RESULT_OK && job.instance)
            WaitForJob(job.instance);

        MI_Instance_Delete(params);
        MI_Operation_Close(&op);
        MI_Operation_Close(&classOp);
        return GetLatestPartition(disk);
    }

    MI_Instance* GetLatestPartition(MI_Instance* disk) {
        MI_Value path;
        MI_Instance_GetElement(disk, MI_T("Path"), &path, nullptr, nullptr, nullptr);
        wstring q = L"ASSOCIATORS OF {" + wstring(path.string) + L"} WHERE AssocClass=MSFT_DiskToPartition ResultClass=MSFT_Partition";
        MI_Operation op = MI_OPERATION_NULL;
        MI_Session_QueryInstances(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("WQL"), q.c_str(), nullptr, &op);
        const MI_Instance* p = nullptr; MI_Instance* lp = nullptr; MI_Boolean m;
        while (MI_Operation_GetInstance(&op, &p, &m, nullptr, nullptr, nullptr) == MI_RESULT_OK && p) {
            if (lp) MI_Instance_Delete(lp);
            MI_Instance_Clone(p, &lp);
        }
        MI_Operation_Close(&op);
        return lp;
    }

    void FormatPartition(MI_Instance* part, const wstring& fs, const wstring& label) {
        cout << "Step 3: Formatting Volume..." << endl;
        MI_Value path;
        MI_Instance_GetElement(part, MI_T("Path"), &path, nullptr, nullptr, nullptr);
        wstring q = L"ASSOCIATORS OF {" + wstring(path.string) + L"} WHERE AssocClass=MSFT_PartitionToVolume ResultClass=MSFT_Volume";
        MI_Operation op = MI_OPERATION_NULL;
        MI_Session_QueryInstances(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("WQL"), q.c_str(), nullptr, &op);
        const MI_Instance* v = nullptr; MI_Boolean m;
        MI_Operation_GetInstance(&op, &v, &m, nullptr, nullptr, nullptr);
        if (!v) throw runtime_error("Volume assoc fail");

        MI_Instance* vClone; MI_Instance_Clone(v, &vClone);
        MI_Operation_Close(&op);

        MI_Operation cop = MI_OPERATION_NULL; const MI_Class* vc = nullptr;
        MI_Session_GetClass(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("MSFT_Volume"), nullptr, &cop);
        MI_Operation_GetClass(&cop, &vc, &m, nullptr, nullptr, nullptr);
        MI_Instance* p = nullptr;
        MI_Application_NewInstanceFromClass(&app, MI_T("MSFT_Volume"), vc, &p);
        MI_Value val;
        val.string = (MI_Char*)fs.c_str(); MI_Instance_SetElement(p, MI_T("FileSystem"), &val, MI_STRING, 0);
        val.string = (MI_Char*)label.c_str(); MI_Instance_SetElement(p, MI_T("FileSystemLabel"), &val, MI_STRING, 0);
        val.boolean = MI_FALSE; MI_Instance_SetElement(p, MI_T("Full"), &val, MI_BOOLEAN, 0);

        MI_Operation fop = MI_OPERATION_NULL;
        MI_Session_Invoke(&session, 0, nullptr, STORAGE_NAMESPACE, MI_T("MSFT_Volume"), MI_T("Format"), vClone, p, nullptr, &fop);
        const MI_Instance* fout = nullptr;
        MI_Operation_GetInstance(&fop, &fout, &m, nullptr, nullptr, nullptr);
        CheckMethodOutput(fout, "Format");

        MI_Instance_Delete(p); MI_Instance_Delete(vClone);
        MI_Operation_Close(&fop); MI_Operation_Close(&cop);
    }
};

int main(int argc, char* argv[]) {
    int diskIdx = 2; // 默认测试磁盘 2
    try {
        StorageManager sm;
        sm.Connect();
        MI_Instance* disk = sm.GetDisk(diskIdx);

        sm.ClearDisk(disk); // 先清理
        this_thread::sleep_for(chrono::seconds(1)); // 给系统一点反应时间

        MI_Instance* part = sm.CreatePartition(disk, 0); // 0 代表使用最大可用空间
        sm.FormatPartition(part, L"NTFS", L"WORK_DATA");

        cout << "\n[Success] Disk " << diskIdx << " has been partitioned and formatted." << endl;
        MI_Instance_Delete(disk); MI_Instance_Delete(part);
    }
    catch (const exception& e) {
        cerr << "\n[Fatal] " << e.what() << endl;
        return -1;
    }
    return 0;
}