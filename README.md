# m5stack-tab5-esp32-p4-tab5book
[WIP] m5stack tab5 ebook reader by DeepSeek AI V41 Flash

# Tab5 Book Reader

面向 **M5Stack Tab5（ESP32-P4）** 的本地电子书阅读器，基于 **ESP-IDF 5.5.6 + LVGL 9.6 + M5Stack Tab5 BSP**。

支持从 SD 卡浏览并阅读：

| 格式 | 支持情况 | 说明 |
| --- | --- | --- |
| **TXT** | 完整支持 | UTF-8 / UTF-16（BOM）/ GBK / GB18030 双字节；自动分页 |
| **EPUB** | 文本模式 | ZIP 解压、OPF/Spine 解析、NCX 目录、XHTML 转文本、章节跳转 |
| **PDF** | 文本模式 | 解析 xref/对象流/字体/ToUnicode，逐页提取文字后重新排版 |

> PDF 采用“文字层提取 + 重排”的方式，而不是完整的光栅化渲染。
> 有文字层的 PDF（大多数电子书、办公文档）可以正常阅读；扫描版 PDF、
> 纯图片 PDF 没有文字层，会显示提示文字。加密 PDF 暂不支持。
>
> EPUB 当前只提取正文文字，不显示图片、不执行 CSS；带 DRM 的 EPUB 不支持。

---

## 功能

- 5 英寸 1280×720 横屏阅读界面，24 px 中文点阵字体（GB2312 全集 + 常用扩展符号）
- SD 卡文件浏览器：目录进入/返回、刷新、按目录优先排序、显示文件类型和大小
- 阅读器：
  - 左右点击翻页，中间点击显示/隐藏工具栏，支持左右滑动翻页
  - 顶部：返回、书名、格式标签、主题、目录/跳转
  - 底部：页码、章节名、阅读进度条、上一页/下一页
  - EPUB：章节目录列表
  - PDF：按 PDF 页码跳转
  - TXT：按全书百分比跳转
- 三种主题：日间 / 护眼 / 夜间；屏幕亮度调节
- 阅读进度与最近阅读记录保存在 NVS，重启后继续阅读
- 最近阅读列表（最多 8 本）
- 中文字体直接编入固件，不依赖 SD 卡字体文件

---

## 硬件与软件

- **开发板**：M5Stack Tab5（ESP32-P4，16 MB Flash，32 MB PSRAM，720×1280 MIPI-DSI 屏，GT911 触摸）
- **ESP-IDF**：v5.5.x（本项目在 **v5.5.6** 上验证）
- **屏幕方向**：代码启动时旋转 90°，使用 1280×720 横屏
- **存储**：MicroSD 卡（FAT32/exFAT，建议 FAT32）

---

## 快速开始

### 1. 准备 ESP-IDF 环境

以本机安装路径为例：

```powershell
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.6'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:PATH = "D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;$env:PATH"
. "$env:IDF_PATH\export.ps1"

cd D:\work_dsh\tab5book
```

也可以直接双击/运行 `tools\idf_shell.bat` 打开一个已配置好的 ESP-IDF 命令行。

### 2. 编译

```powershell
idf.py set-target esp32p4   # 第一次构建时执行
idf.py build
```

### 3. 烧录并查看日志

```powershell
idf.py -p COMx flash monitor
```

Tab5 使用 USB-C 口的 USB-Serial/JTAG 作为下载和日志口；`sdkconfig.defaults`
已把控制台配置为 USB-Serial/JTAG。

### 4. 放置书籍

把书籍复制到 SD 卡根目录或任意子目录，例如：

```
SD 卡根目录/
├─ books/
│  ├─ 三体.epub
│  ├─ 示例文档.pdf
│  └─ 荷塘月色.txt
└─ 其他目录/
   └─ ...
```

开机后进入桌面 → “打开 SD 卡” → 浏览到书籍 → 点击打开。
`samples/` 目录里提供了三个测试文件，可直接复制到 SD 卡：

- `荷塘月色.txt`（UTF-8）
- `荷塘月色_GBK.txt`（GBK 编码测试）
- `示例书籍.epub`（两章 + NCX 目录）
- `示例文档.pdf`（中文 Type0/ToUnicode 页 + 英文 Helvetica 页）

---

## 操作说明

### 桌面

- **打开 SD 卡**：进入文件浏览器
- **继续阅读**：打开上次的书
- **最近阅读**：显示最近 5 本（右侧面板）
- **设置**：主题、亮度、清除阅读记录
- **关于**：版本与格式说明

### 文件浏览器

- **主页**：回桌面
- **上一级**：返回上级目录
- **刷新**：重新扫描当前目录
- 点击目录进入；点击 `.txt` / `.md` / `.epub` / `.pdf` 打开

### 阅读器

| 操作 | 效果 |
| --- | --- |
| 点击屏幕左侧 240 px | 上一页 |
| 点击屏幕右侧 240 px | 下一页 |
| 点击屏幕中间 | 显示/隐藏上下工具栏 |
| 左右滑动 | 上一页 / 下一页 |
| 底部“上一页/下一页” | 翻页 |
| 顶部“目录” | EPUB 章节列表 |
| 顶部“跳转” | PDF 页码 / TXT 百分比 |
| 顶部“主题” | 日间 → 护眼 → 夜间循环 |
| 顶部“返回” | 保存进度并回到文件浏览器 |

---

## 项目结构

```
tab5book/
├─ CMakeLists.txt
├─ partitions.csv                 # 8 MB 应用分区
├─ sdkconfig.defaults             # Tab5 / PSRAM / USB 控制台 / LVGL 配置
├─ main/
│  ├─ app_main.c                  # 显示、触摸、SD、UI 启动
│  ├─ idf_component.yml           # m5stack_tab5 BSP + zlib
│  ├─ reader/
│  │  ├─ doc.c / doc.h            # 统一文档接口
│  │  ├─ text.c / text.h          # UTF-8/UTF-16/GBK 解码、CJK 分行与分页
│  │  ├─ txt.c                    # TXT 文档
│  │  ├─ zip.c / zip.h            # 最小 ZIP 读取器（stored/deflate）
│  │  ├─ epub.c                   # EPUB 容器/OPF/NCX/XHTML
│  │  ├─ pdf.c                    # PDF 对象/xref/stream/font/ToUnicode
│  │  ├─ pdf_text.c               # PDF 内容流文本提取与重排
│  │  └─ gbk_table.h              # GBK→Unicode 表（自动生成）
│  ├─ storage/
│  │  └─ library.c                # NVS 最近阅读/进度/主题/亮度
│  └─ ui/
│     ├─ ui.c                     # 桌面、文件浏览器、设置、对话框
│     └─ ui_reader.c              # 阅读器界面、目录、跳转
├─ components/book_font/
│  ├─ cjk_font_24.c               # 24 px CJK LVGL 字体（生成文件）
│  └─ book_font.c                 # 行高调整后的字体封装
├─ samples/                       # 测试书籍
└─ tools/
   ├─ gen_charset.py              # 生成字符集 + GBK 表
   ├─ gen_font.js                 # 调 lv_font_conv 生成字体 C 文件
   ├─ gen_pdf_tables.py           # 生成 PDF 编码/字形名表
   └─ make_samples.py             # 生成测试 TXT/EPUB/PDF
```

---

## 字体与字符集

`components/book_font/cjk_font_24.c` 由 **Noto Sans SC** 生成，24 px、4 bpp，
覆盖：

- ASCII / Latin-1
- GB2312 全部汉字（6763 字）
- 常用标点、全角符号、箭头、几何图形、部分姓名/生僻字

生成步骤：

```bash
# 1) 字符集与 GBK 表
python tools/gen_charset.py

# 2) 安装 lv_font_conv（Node.js）
npm install lv_font_conv@1.5.3

# 3) 生成字体（Windows 上通过 wrapper 避免命令行长度限制）
set FONT_PATH=C:\Windows\Fonts\NotoSansSC-VF.ttf
set LV_FONT_CONV=C:\path\to\node_modules\lv_font_conv\lv_font_conv.js
node tools/gen_font.js
```

字体对象约 1.4 MB（编译后，RLE 压缩），会占用应用分区的一部分。当前应用分区为 8 MB；
如需更大字体或更多字形，可调整 `partitions.csv`。

字体许可：Noto Sans SC 采用 SIL Open Font License 1.1。

---

## PDF 阅读说明

实现的是一个面向嵌入式设备的轻量 PDF 文本阅读器：

- 支持 xref 表 / xref 流 / 对象流（ObjStm）
- 支持 FlateDecode（含 PNG/TIFF 预测器）、ASCIIHex、ASCII85、RunLength
- 支持 Type0（Identity-H）/ Type1 / TrueType 字体
- 优先使用 `/ToUnicode` CMap；简单字体回退到 WinAnsi/MacRoman/Standard 编码与 `/Differences`
- 支持 `BT/ET`、`Tf`、`Tm`、`Td`、`TD`、`T*`、`Tj`、`TJ`、`'`、`"`、`TL`、`Tc`、`Tw`、`Tz`、`Ts`、`cm`、`q/Q`
- 不支持：加密 PDF、扫描图片页、嵌入图像渲染、完整矢量图形渲染

因此：

- 文字版 PDF：可读，按阅读器宽度重新排版，保留段落/行间距
- 扫描版 PDF：会显示“本页没有可提取的文字，可能是扫描版 PDF”
- 使用未嵌入字体且没有 ToUnicode 的少见 PDF：可能丢失部分文字

---

## 已知限制

- TXT 文件上限 8 MB；PDF 文件上限 16 MB（受 PSRAM/解析时间限制）
- EPUB 不支持 DRM、图片、CSS 排版；只提取文字
- 字体为 GB2312 字符集，极少数生僻字/繁体字可能显示为缺字
- 目录最多显示前 150 章
- 阅读进度按“章 + 页”保存；修改主题不会改变分页，重启后仍能恢复
- 屏幕旋转使用 BSP 的软件旋转；如后续 BSP 支持硬件旋转可关闭 `sw_rotate`

---

## 构建验证

本项目已完成 `idf.py build` 编译验证（ESP-IDF v5.5.6，target `esp32p4`）。

本机验证结果：

| 项目 | 数值 |
| --- | --- |
| `build/tab5book.bin` | 约 **2.56 MB** |
| 应用分区 | 8 MB（剩余约 70%） |
| DIRAM 静态占用 | 378,622 B / 65.68%，剩余约 193 KB |
| 内置 CJK 字体 | 约 1.39 MB（Flash `.rodata`） |
| LVGL | 约 651 KB |

可查看详细占用：

```powershell
idf.py size
idf.py size-components
```

若应用超过 8 MB，可：

1. 调大 `partitions.csv` 中 `factory` 分区（Flash 共 16 MB，当前已用 8 MB）
2. 减小字体字号/字符集（`tools/gen_charset.py` + `tools/gen_font.js`）
3. 关闭 LVGL 中未使用的库（`sdkconfig` 中 `LV_USE_*`）

---

## 故障排查

| 现象 | 处理 |
| --- | --- |
| 屏幕黑屏 | 确认使用的是 `esp32p4` target；确认 BSP 初始化日志；检查背光/供电 |
| 触摸方向不对 | 确认调用了 `bsp_display_rotate(..., LV_DISPLAY_ROTATION_90)`；BSP 会自动同步触摸坐标 |
| 无法挂载 SD 卡 | 使用 FAT32；重新插卡；查看串口 `SD card mount` 日志 |
| 中文显示为方框 | 确认 `components/book_font/cjk_font_24.c` 已参与编译；重新生成字体 |
| PDF 打开失败 | 可能为加密 PDF 或文件过大；串口会打印具体错误 |
| EPUB 打开失败 | 可能为 ZIP64/DRM/损坏文件；尝试用 Calibre 重新导出 |
| 构建时字体文件编译很慢 | 字体 C 文件约 8.7 MB，首次编译需要一些时间；增量构建会复用对象文件 |

---

## 许可

- 项目代码：MIT（可按需修改）
- Noto Sans SC 字体：SIL Open Font License 1.1
- 示例文本《荷塘月色》：朱自清，公共领域
- 示例 EPUB/PDF：由 `tools/make_samples.py` 生成，可自由使用
