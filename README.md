# 项目配置说明

本项目已配置使用 clangd、CMake 和 CodeLLDB 进行开发和调试。

## 环境要求

确保已安装以下工具：

1. **clangd** - C/C++ 语言服务器
2. **CMake** (>= 3.10) - 构建系统
3. **Ninja** - 构建工具
4. **CodeLLDB** - VSCode 扩展，用于调试
5. **clangd** - VSCode 扩展

## 安装 VSCode 扩展

1. **clangd** (llvm-vs-code-extensions.vscode-clangd)
2. **CodeLLDB** (vadimcn.vscode-lldb)
3. **CMake Tools** (可选，ms-vscode.cmake-tools)

## 使用步骤

### 1. 配置项目

在终端执行：
```powershell
cmake -B build -S . -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

或使用 VSCode 任务：
- 按 `Ctrl+Shift+P`
- 输入 "Tasks: Run Task"
- 选择 "CMake: Configure (Debug)"

### 2. 编译项目

在终端执行：
```powershell
cmake --build build --config Debug -j 8
```

或使用 VSCode：
- 按 `Ctrl+Shift+B`（默认构建任务）
- 或选择 "CMake: Build"

### 3. 调试项目

两种方式：

**方式1：使用 F5 快捷键**
- 直接按 `F5`，会自动编译并启动调试

**方式2：通过调试面板**
- 点击左侧调试图标
- 选择 "Debug with CodeLLDB"
- 点击绿色播放按钮

### 4. 其他有用的任务

- **CMake: Clean** - 清理构建文件
- **CMake: Configure and Build** - 配置并构建
- **CMake: Rebuild** - 重新构建

## clangd 配置

clangd 会自动读取 `build/compile_commands.json` 文件，提供：
- 代码补全
- 语法检查
- 代码导航
- 重构支持
- 实时诊断

## 调试配置

### 可用的调试配置：

1. **Debug with CodeLLDB** - 先编译后调试
2. **Debug (without build)** - 直接调试（不编译）
3. **Attach to Process** - 附加到正在运行的进程

### 调试技巧：

- 设置断点：在代码行号左侧点击
- 条件断点：右键断点设置条件
- 查看变量：鼠标悬停或使用"变量"面板
- 调用栈：查看"调用栈"面板
- 监视表达式：在"监视"面板添加

## 常见问题

### clangd 无法找到头文件？

确保已经运行过 CMake 配置生成 `compile_commands.json`：
```powershell
cmake -B build -S . -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
```

### 编译失败？

1. 检查 FFmpeg 路径是否正确（CMakeLists.txt 第15行）
2. 确保 Ninja 已安装并在 PATH 中
3. 确保使用的编译器支持 C++20

### 调试无法启动？

1. 确保已经编译成功
2. 检查 `build/ffmpeg-merge.exe` 是否存在
3. 检查 CodeLLDB 扩展是否正确安装

## 项目结构

```
merge/
├── .vscode/           # VSCode 配置
│   ├── settings.json  # 编辑器设置
│   ├── tasks.json     # 构建任务
│   └── launch.json    # 调试配置
├── .clangd            # clangd 配置
├── build/             # 构建输出目录
│   └── compile_commands.json  # clangd 使用的编译数据库
├── CMakeLists.txt     # CMake 构建脚本
├── CMakeSettings.json # Visual Studio 配置文件
└── win-thread.cpp     # 源代码
```

## 键盘快捷键

- `Ctrl+Shift+B` - 编译项目
- `F5` - 开始调试
- `Ctrl+F5` - 运行（不调试）
- `F9` - 切换断点
- `F10` - 单步跳过
- `F11` - 单步进入
- `Shift+F11` - 单步跳出
- `Shift+F5` - 停止调试
