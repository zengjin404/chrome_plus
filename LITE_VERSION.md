# Chrome++ 增进工坊定制精简版 (Lite)

本分支（`lite`）是基于 [Chrome++ Next](https://github.com/Bush2021/chrome_plus) 深度精简与定制的专用版本。

---

## 💡 定制背景与核心改进

官方版 Chrome++ 功能丰富，但附带的功能和模块较多，在特定使用场景下存在部分痛点。本项目针对以下关键问题进行了彻底改造：

1. **专注单一核心能力（双击关闭标签页）**
   - 剔除所有冗余功能（如手势、老板键、网页翻译、便携重定向、按键映射等），仅保留基于 Windows UI Automation 框架实现的**双击标签页关闭**。
   - 自动识别并避开标签页自身的自带关闭按钮，避免误触。
2. **彻底修复“清除浏览数据”卡死 Bug**
   - 官方版在使用快捷键 `Ctrl+Shift+Delete` 打开清除浏览数据弹窗时，缓存及数据大小会永远停留在“正在计算...”状态。
   - 本精简版通过剥离冗余的系统级钩子与重定向逻辑，彻底消除了该 Bug，浏览器计算与清理功能完全恢复原生常态。
3. **零路径侵入，保持 Chrome 官方默认 Profile**
   - 彻底移除了 `portable.cc` 与 `green.cc` 便携化路径重定向。
   - 完全不劫持 `User Data` 和缓存路径，继续使用 Chrome 官方默认的标准个人配置文件夹，免除因相对路径重定向导致的数据混乱风险。
4. **纯净单文件，零配置依赖**
   - 移除了外部 `chrome++.ini` 配置文件的依赖，内部代码无需读取任何配置文件。
   - 打包构建流程只输出单一文件 `version.dll`，直接放入 Chrome 程序根目录即可直接使用。
5. **原生网页级关于页融入**
   - 在 Chrome 设置的“关于 Chrome”页面（`chrome://settings/help`）版本号下方无缝呈现：
     > 双击关闭标签页 功能由 [增进工坊](https://zengjin.work) 强力驱动
   - 基于 Chromium `resources.pak` 内存写时复制（`PAGE_WRITECOPY`）与跨进程共享段补丁技术，直接复用 Chrome 原生 `class="secondary"` 样式表。
   - 100% 杜绝了外挂 Win32 浮层窗口在高分屏 DPI 缩放下抖动频闪、以及覆盖开发者工具（DevTools）控制台的兼容性问题。

---

## 🚀 编译与获取方式

### 方式一：GitHub Actions 自动构建（推荐）
本项目已配置 GitHub Actions 自动化编译流水线：
1. 每次向 `lite` 分支推送代码，GitHub Actions 会自动拉起 Windows Server 干净环境并使用 Clang + Ninja 完成编译。
2. 前往仓库页面顶部的 **[Actions](https://github.com/zengjin404/chrome_plus/actions)** 标签页。
3. 点击最新的一条 Workflow 运行记录。
4. 在页面底部的 **Artifacts** 区域即可直接下载编译产物（如 `chrome_plus-x64-release` 等压缩包）。
5. 解压后将 `version.dll` 放置于 `chrome.exe` 同级目录下，重新启动 Chrome 即可生效。

### 方式二：本地编译
**环境要求**：
- Windows 10 / 11 64位
- Visual Studio 2022（需勾选 `使用 C++ 的桌面开发` 及 `适用于 Windows 的 Clang 工具`）
- CMake 3.29+ 与 Ninja

**编译步骤**：
```cmd
# 默认构建 x64 Release 版本
build.cmd

# 或指定工具链与架构
build.cmd ninja x64 release
```
编译产物位于 `out\x64\release\version.dll`。

---

## 🔄 后续同步上游（Upstream）更新指南

若 Chrome++ 官方仓库后续针对 Chrome 新内核发布了重要底层适配修复，可通过以下步骤安全同步：

### 1. 配置上游远程仓库
在本地仓库根目录下运行：
```bash
git remote add upstream https://github.com/Bush2021/chrome_plus.git
git fetch upstream
```

### 2. 查看或同步指定更新
建议采用 `cherry-pick` 挑拣特定修复提交，或使用 `git merge` 合并主分支：
```bash
# 查看上游最新提交记录
git log upstream/main --oneline -n 10

# 推荐：挑选特定针对 Chromium 适配的修复提交
git cherry-pick <commit-hash>
```

### 3. 合并冲突时的注意事项
若上游修改涉及以下文件，请注意保留精简版修改：
- [src/chrome++.cc](src/chrome++.cc)：需维持仅调用 `TabBookmark()`、`InstallInputHooks()` 与 `PakPatch()`，不要恢复 `green.cc` 或 `portable.cc`。
- [src/pakpatch.cc](src/pakpatch.cc)：保留自定义的文案与跳转链接。
- [CMakeLists.txt](CMakeLists.txt)：保留移除了冗余模块、仅保留 `tabbookmark`、`uia`、`inputhook`、`pakfile`、`pakpatch`、`mini_gzip` 的目标定义。
- [build.cmd](build.cmd)：保留打包步骤中不再复制 `chrome++.ini` 的修改。

合并确认无误后，再次推送到你的 `lite` 分支：
```bash
git push origin lite
```
GitHub Actions 将自动为你编译出适配最新 Chromium 底层的精简版 DLL。
