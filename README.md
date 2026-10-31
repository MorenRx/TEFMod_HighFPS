# HighFPS - 安卓端Terraria高帧率与运动插值模组

基于 **TEFKernelLoader** 开发的安卓端Terraria（Android ARM64，适配 1.4.5+）原生高帧率增强模组。

---

## 🔨 源码构建指南

本项目支持 Windows / Linux 跨平台交叉编译 Android ARM64 架构。

### 克隆源码
```bash
git clone --recurse-submodules https://github.com/MorenRx/TEFMod_HighFPS.git
```
如果已克隆未包含子模块，可执行：
```bash
git submodule update --init --recursive
```

### 前置依赖
- **Android NDK**：r25c 或以上（本项目使用 NDK r26b / 26.1.10909125）
- **CMake**：3.22+
- **Ninja**

### 一键编译（PowerShell）
```powershell
./build.ps1 -BuildType Release
```
脚本将自动配置 CMake、编译生成 `libhighfps.android.arm64.so`、剥离调试符号，并自动打包生成 `dist/HighFPS_TEFKL_android_arm64.zip`。

---

## 📜 模组信息
- **模组名称**：HighFPS
- **模组包名**：`top.morenrx.highfps`
- **上级加载器**：`eternal.future.kernelloader` (KernelLoader)
- **目标平台**：Android ARM64 (aarch64-linux-android)
- **适用游戏版本**：泰拉瑞亚 1.4.5+
