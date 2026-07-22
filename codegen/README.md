# Codegen

## 已锁定工具链（`versions.lock`）

| 组件 | 版本 | 来源 |
|------|------|------|
| **Python** | **3.13.13** | python-build-standalone `20260414` / `install_only_stripped` |
| **libclang** | **libclang-ng 22.1.4.2** | PyPI wheel（含原生 libclang） |

包内只保留 `versions.lock` + 脚本；**不**把 Python 放进仓库或业务工程。

## 用户级 cache（按 lock 指纹分目录）

默认根目录：

| OS | 路径 |
|----|------|
| Windows | `%LOCALAPPDATA%\dart_cpp_bridge\toolchain` |
| macOS | `~/Library/Caches/dart_cpp_bridge/toolchain` |
| Linux | `${XDG_CACHE_HOME:-~/.cache}/dart_cpp_bridge/toolchain` |

覆盖：环境变量 `DCB_CODEGEN_CACHE`。

```text
<cache>/
  downloads/
    <sha256>.tar.gz      # Python 包，内容寻址，多 lock 共用
    <sha256>.whl         # libclang-ng wheel
  envs/
    <platform>-<fp16>/   # fp = sha256(platform|py_sha|lc_sha) 前 16 位
      python/
      READY.json
  LAST_ENV.json          # 最近一次 bootstrap 指向的 env
  tmp/                   # 解压临时目录
```

- **同一** `versions.lock`（同平台）→ 同一 `envs/...`，只下一份  
- **不同** lock 哈希 → 不同文件夹，互不覆盖  
- `downloads/` 按 blob sha 去重  

清理：删整个 cache 根或某个 `envs/<key>/` 即可；下次 codegen 会重建。

## 用法

```powershell
cd codegen
.\codegen.ps1
.\codegen.ps1 -Force
```

```bash
cd codegen
chmod +x codegen.sh bootstrap.sh
./codegen.sh
./codegen.sh --force
```

## 用户工程约定（设计已锁定，实现未做）

见 [docs/frb_and_cpp_bridge_design.md](../docs/frb_and_cpp_bridge_design.md) §6.1：

- 工程根 **`dart_cpp_bridge.yaml`**：`scan` / `dart_output` / `cpp_wire_output` / `include_paths`
- 只扫 `scan` 目录下的 `.h` / `.hpp`
- **仅**带生成标记的声明导出：`BRIDGE_SYNC` / `ASYNC` / `NORMAL` / `EXPORT`，或参数含 `StreamSink<T>`
- 无标记 → 忽略（可同文件存在）

## 流水线状态

| 步骤 | 状态 |
|------|------|
| `versions.lock` | ✅ |
| 用户 cache + lock 指纹 env | ✅ |
| bootstrap / smoke | ✅ |
| 配置 yaml + 扫目录 + 按标记过滤 | 📄 设计已写，代码未做 |
| 解析 API → 生成 wire/Dart | ❌ |
