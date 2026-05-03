## HSR 「货币战争」 环境刷取工具

> 前提情要：  
> 由于作者游玩HSR时被高难度货币战争的逆天环境吓哭，遂与AI合作开发此工具，帮助自己和其他玩家自动刷取理想的投资环境。
> 此版本通过C++重写，减少了 Python 版本过于笨重的问题，并对GUI相关逻辑进行了优化。

HSR「货币战争」自动刷取工具。通过 PaddleOCR 识别投资环境与 Debuff，自动循环进入 → 识别 → 筛选 → 退出重开，直到匹配目标策略后停止并通知。


> [!WARNING]
> 内置 Debuff 列表基于当前版本整理，游戏更新后可能出现未收录的新 Debuff，导致识别遗漏。
> 如果你发现工具未能识别某个 Debuff，请 [提交 Issue](https://github.com/YsKiKi/CurrencyWar_Cpp/issues/new?template=new-debuff.yml) 并附上截图，帮助我们及时更新列表。
> 当前版本 Debuff 列表完全未完全覆盖

### 功能

- **自动循环**：开始货币战争 → 进入标准博弈 → 选择投资环境 → 不满意则退出重开
- **OCR 识别**：基于 PaddleOCR 识别当前投资策略与 Debuff 文本
- **模板匹配**：OpenCV 模板匹配识别游戏按钮状态
- **策略筛选**：支持设置目标策略、不想要的 Debuff、需要的 Buff
- **蓝海支持**：自动进入蓝海界面选择策略。赌一把！万一出了白嫖6金币
- **可视化覆盖层**：实时显示 OCR 识别结果与当前步骤
- **Qt6 GUI**：搜索联想选择策略/Debuff、框选识别区域、快捷键捕获
- **快捷键停止**：支持自定义组合键（Ctrl/Alt/Shift+键 或单键）

### 开始使用

> [!IMPORTANT]
> - 本工具中的button图片为2560x1440分辨率截图，如果你的设备使用不同分辨率，建议自行截图替换 `res/buttons/` 目录下对应的图片
> - 请勿修改对应的图片命名，否则程序将无法识别按钮状态

#### 从 Releases 下载
访问 [Releases](https://github.com/YsKiKi/CurrencyWar_Cpp/releases/latest) ，下载最新版本的 `CurrencyWar.zip`，解压后运行 `CurrencyWar.exe`


#### 自行编译
1. 将本仓库克隆到本地：
```bash
git clone https://github.com/YsKiKi/CurrencyWar_Cpp.git
```
2. 安装依赖项：
   - C++17 编译器（如 Visual Studio 2022）
   - CMake 3.20+
   - OpenCV 4.5+
   - PaddleOCR C++ 库
   - Qt6（可选，用于 GUI）
3. 运行 `build_all.ps1` 脚本进行编译，生成 `build/` 目录，运行编译完成的 `CurrencyWar.exe`

#### 运行流程
1. 选择目标策略和Debuff。  
2. 切换标签页，设置OCR识别次数，框选识别区域，配置快捷键。
3. 启动！


> [!NOTE]
> - 程序启动时会自动请求管理员权限（UAC），HSR为高权限窗口，必须以管理员权限运行才能正确截图和点击
> - 需要先启动HSR并进入货币战争界面
> - OCR 识别区域需根据实际分辨率通过 GUI 框选调整

### License

[GPL-3.0 License](./LICENSE)