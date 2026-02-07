# Windows 磁盘分区与格式化工具（Storage Management API / SMAPI）

> 目标：在 **Windows 8+** 环境通过 Storage 模块（基于 SMAPI）自动完成 GPT 初始化、分区创建与格式化。

## 1. 方案说明

本项目已切换为 **SMAPI 方案**（不再使用旧版 VDS COM）。

- CLI 负责解析参数（`--disk`、`--gpt`、`--create-part`、`--format`）。
- 程序将任务编译为一段 PowerShell Storage 脚本并执行：
  - `Initialize-Disk`
  - `New-Partition`
  - `Format-Volume`

这样可直接使用 Windows 新版存储管理栈，兼容现代系统和维护路径。

## 2. 命令行参数

- `--disk=1`：目标磁盘号。
- `--gpt`：初始化为 GPT。
- `--create-part=...`（可重复）
  - `size=10G`（必填）
  - `offset=1M`（可选）
  - `label=MyPart`（可选；SMAPI 的 `New-Partition` 不直接设置 GPT 名称，当前会给出提示）
  - `type=basic` 或 GPT type GUID（可选）
- `--format=...`（可选，且必须跟在某个 `--create-part` 后）
  - `fs=ntfs|fat32|exfat`
  - `vol=Data`
  - `quick=1|0`

示例：

```powershell
./disk_part_fmt.exe \
  --disk=1 --gpt \
  --create-part=size=10G,type=basic \
  --format=fs=ntfs,vol=Data,quick=1
```

## 3. 构建

```powershell
cmake -S . -B build -G "Ninja"
cmake --build build -c Release
```

> 仅支持 Windows 构建与运行。
