# Windows 磁盘分区与格式化工具（Storage Management / VDS COM 示例）

> 目标：提供一个可扩展的工程骨架，用于在 **Windows 8+** 环境中自动完成磁盘 GPT 初始化、分区创建、分区命名以及文件系统格式化。

## 1. 架构说明

本示例采用单进程 CLI + COM 组件化调用：

1. **参数层（ParseArgs）**
   - 解析 `--disk`、`--gpt`、`--create-part`、`--format`。
   - 将参数转换为 `CliOptions / PartitionRequest / FormatOptions`。
2. **存储操作层（VDS Service）**
   - `ConnectVdsService()`：连接 VDS 服务。
   - `FindDisk()`：枚举 Provider / Pack / Disk，定位目标磁盘号。
   - `InitializeDiskGpt()`：将磁盘初始化为 GPT。
   - `BuildCreateParams()`：构造 `CREATE_PARTITION_PARAMETERS` 并设置 `Gpt.name`。
   - `FormatVolume()`：通过 `IVdsVolumeMF::Format` 执行格式化。
3. **异步控制层（WaitAsync）**
   - 对 `IVdsAsync` 统一等待、统一 `HRESULT` 错误处理。

---

## 2. 命令行参数设计

支持以下参数：

- `--disk=1`：目标磁盘号（PhysicalDrive1）。
- `--gpt`：对目标盘执行 GPT 初始化。
- `--create-part=...`：创建分区（可重复）。
  - `size=10G`（必须）
  - `offset=1M`（可选）
  - `label=MyPart`（可选，对应 GPT PartLabel / Name）
  - `type=basic`（可选，默认 Basic Data；也可传完整 GUID）
- `--format=...`：对最近创建分区对应卷执行格式化（可选，需跟在某个 `--create-part` 后）
  - `fs=ntfs|fat32|exfat`
  - `vol=Data`
  - `quick=1|0`

示例：

```powershell
# 创建一个 10G NTFS 分区
./disk_part_fmt.exe \
  --disk=1 --gpt \
  --create-part=size=10G,label=MyPart,type=basic \
  --format=fs=ntfs,vol=Data,quick=1

# 创建两个分区（第二个手动指定偏移）
./disk_part_fmt.exe \
  --disk=2 --gpt \
  --create-part=size=4G,label=Boot,type=basic \
  --format=fs=fat32,vol=BOOT,quick=1 \
  --create-part=size=200G,offset=5G,label=Data,type=basic \
  --format=fs=ntfs,vol=DATA,quick=1
```

---

## 3. 关键 API 解释

1. **COM 初始化**
   - `CoInitializeEx(COINIT_MULTITHREADED)`
   - `CoInitializeSecurity(...)`
2. **服务连接与枚举**
   - `IVdsServiceLoader::LoadService`
   - `IVdsService::WaitForServiceReady`
   - `IVdsService::QueryProviders -> IVdsSwProvider::QueryPacks -> IVdsPack::QueryDisks`
3. **GPT 初始化**
   - `IVdsAdvancedDisk::ConvertStyle(VDS_PST_GPT, ...)`
4. **创建 GPT 分区**
   - `IVdsAdvancedDisk::CreatePartition`
   - `CREATE_PARTITION_PARAMETERS`
     - `style = VDS_PST_GPT`
     - `PartitionStyle.Gpt.PartitionType`
     - `PartitionStyle.Gpt.name`（即 GPT Name 字段）
5. **格式化**
   - `IVdsVolumeMF::Format`
   - 支持 NTFS/FAT32/exFAT + 卷标 + quick format。
6. **异步等待**
   - `IVdsAsync::Wait(&hrResult, &output)`
   - 注意需要检查两层返回：调用本身 `HRESULT` + 任务结果 `hrResult`。

---

## 4. 常见错误处理建议

1. **权限不足（E_ACCESSDENIED）**
   - 必须以管理员权限启动。
2. **磁盘被占用 / 卷正在使用**
   - 关闭资源管理器、杀掉占用进程、卸载卷后重试。
3. **分区对齐问题**
   - 推荐 1MiB 对齐（示例默认 `align = 1MiB`）。
4. **对象尚未刷新（创建后立即格式化失败）**
   - 可增加重试逻辑，等待卷对象刷新。
5. **参数无效（E_INVALIDARG）**
   - 检查 size / offset / GUID 以及文件系统名称。

---

## 5. 最小可编译工程结构

```text
partitionAndFormatOnWindows/
├─ CMakeLists.txt
├─ README.md
└─ src/
   └─ main.cpp
```

构建（Windows + Visual Studio / Ninja）：

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build -c Release
```

---

## 6. 生产落地建议

- 将“创建分区后定位目标卷”从示例中的“最后一个卷”策略，升级为：
  - 读取分区起始偏移/大小并和卷 Extent 精确匹配。
- 对每一步写审计日志（JSON）便于部署回放。
- 增加 `--dry-run`，先做参数与动作规划，不落盘。
- 对多分区批量任务加入事务式回滚策略（至少提供人工恢复脚本）。

