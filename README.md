# Windows 10+ 磁盘分区与格式化工具（C++ / Storage Management API）

> 本示例基于 **Windows Storage Management API（SMAPI）** 进行实现（通过 Storage 模块 cmdlet 调用系统存储管理栈），**不使用 IOCTL 或 VDS**。

## 一、架构说明

本工程将功能分成三层：

1. **CLI 参数层（C++）**
   - 解析 `--disk`、`--gpt`、`--create-part`、`--format`。
   - 将参数转成结构化配置（`CliConfig` / `PartitionConfig` / `FormatConfig`）。

2. **磁盘发现层（C++ + COM + WMI）**
   - COM 初始化：`CoInitializeEx` / `CoInitializeSecurity`。
   - 连接命名空间：`ROOT\Microsoft\Windows\Storage`。
   - 查询 `MSFT_Disk` 实例完成磁盘枚举并选择目标盘。

3. **执行层（Storage Management API）**
   - 由 C++ 生成并执行 PowerShell Storage 脚本：
     - `Initialize-Disk`（GPT 初始化）
     - `New-Partition`（创建 GPT 分区）
     - `Set-Partition`（尝试设置 GPT 分区名）
     - `Format-Volume`（按参数格式化）
   - 用 `CreateProcessW + WaitForSingleObject` 进行异步等待。

---

## 二、支持能力与参数

### 1) 磁盘初始化 GPT

- `--gpt` 开启 GPT 初始化。
- 命令：`Initialize-Disk -PartitionStyle GPT`。

### 2) 创建 GPT 分区（可多个）

重复传入 `--create-part=...`：

- `size=10G`（必填）
- `offset=1M`（可选）
- `label=MyPart`（可选，尝试设置 GPT PartLabel）
- `type=basic` 或 GUID（可选，默认 Basic Data）

默认分区类型 GUID（Basic Data）：

- `{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}`

### 3) 格式化分区

使用 `--format=...`（作用于它前面的最近一个 `--create-part`）：

- `fs=ntfs|fat32|exfat`
- `vol=Data`
- `quick=1|0`（1 快速格式化，0 全格式化）

---

## 三、命令示例

```powershell
# 磁盘 1：初始化 GPT，创建 10G 基础分区，GPT 名称 MyPart，格式化 NTFS 卷标 Data 快速格式化
.\disk_part_fmt.exe `
  --disk=1 --gpt `
  --create-part=size=10G,label=MyPart,type=basic `
  --format=fs=ntfs,vol=Data,quick=1

# 磁盘 2：创建两个分区
.\disk_part_fmt.exe `
  --disk=2 --gpt `
  --create-part=size=2G,offset=1M,label=EFIData,type={EBD0A0A2-B9E5-4433-87C0-68B6B72699C7} `
  --format=fs=fat32,vol=BOOT,quick=1 `
  --create-part=size=20G,label=Payload,type=basic `
  --format=fs=exfat,vol=PAYLOAD,quick=1
```

---

## 四、关键 API 解释

1. **COM 初始化**
   - `CoInitializeEx`：初始化 COM 线程模型。
   - `CoInitializeSecurity`：设置 WMI/COM 调用安全。

2. **磁盘枚举（Storage 命名空间）**
   - WMI 命名空间：`ROOT\Microsoft\Windows\Storage`
   - 类：`MSFT_Disk`
   - 查询字段：`Number/FriendlyName/Size/PartitionStyle/IsOffline`

3. **GPT 初始化**
   - `Initialize-Disk -Number <N> -PartitionStyle GPT`

4. **创建分区**
   - `New-Partition -DiskNumber <N> -Size <Bytes> -Offset <Bytes> -GptType <GUID>`

5. **设置 GPT 分区 Name（PartLabel）**
   - 尝试：`Set-Partition -NewPartitionName`。
   - 不同 Windows 版本/模块参数可能存在差异，代码中已加兼容警告分支。

6. **格式化**
   - `Format-Volume -Partition <partitionObj> -FileSystem NTFS|FAT32|exFAT -NewFileSystemLabel <Label>`
   - `quick=0` 时添加 `-Full`（全格式化）。

---

## 五、常见错误处理建议

1. **权限不足（Access Denied）**
   - 以管理员权限运行终端。

2. **磁盘被占用（in use）**
   - 先卸载卷、关闭占用进程。
   - 必要时临时离线/只读解除：`Set-Disk -IsOffline $false -IsReadOnly $false`。

3. **分区对齐/偏移不合法**
   - 建议 `offset` 使用 1MiB 对齐（`1M`）。
   - 确保 `size + offset` 不超过磁盘可用范围。

4. **GPT 名称设置失败**
   - 视目标系统 Storage 模块版本而定。
   - 可退化为仅设置 `Format-Volume` 的 `NewFileSystemLabel`。

---

## 六、最小可编译工程结构

```text
partitionAndFormatOnWindows/
├─ CMakeLists.txt
├─ README.md
└─ src/
   └─ main.cpp
```

---

## 七、构建

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build --config Release
```

> 注意：本项目仅可在 Windows 10+ 环境中实际执行分区与格式化逻辑。
