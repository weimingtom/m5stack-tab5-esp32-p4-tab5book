# Tab5 阅读器项目总结

工程已创建在 `D:\work_dsh\tab5book`，并完成 **ESP-IDF v5.5.6 + ESP32-P4** 编译验证。

## 已实现

- **M5Stack Tab5 硬件初始化**
  - BSP：`espressif/m5stack_tab5`（ILI9881C / ST7123、GT911 触摸）
  - 1280×720 横屏、SD 卡挂载、亮度控制
- **文件浏览器**
  - 浏览 `/sdcard` 及子目录，按目录优先排序
  - 支持 `.txt` / `.md` / `.epub` / `.pdf`
- **TXT 阅读器**
  - UTF-8、UTF-16 BOM、GBK/GB18030 双字节自动识别
  - 中文分行、标点禁则、分页、阅读进度
- **EPUB 阅读器**
  - ZIP 解压、`container.xml`、OPF、Spine、NCX 目录
  - XHTML 转纯文本、章节跳转
- **PDF 阅读器**
  - xref 表 / xref 流 / 对象流
  - Flate、ASCIIHex、ASCII85、RunLength 解码
  - Type0/Identity-H、Type1、TrueType 字体
  - `/ToUnicode` CMap、WinAnsi/MacRoman/Standard 编码
  - 逐页提取文字后重新排版
- **阅读界面**
  - 左右点击/滑动翻页，中间点击显示/隐藏工具栏
  - EPUB 目录、PDF 页码跳转、TXT 百分比跳转
  - 日间/护眼/夜间主题、亮度调节
  - NVS 保存最近阅读和阅读进度
- **中文字体**
  - 内置 24 px Noto Sans SC 子集，覆盖 GB2312 6763 字 + ASCII + 常用符号
  - 不依赖 SD 卡字体文件

## 编译结果

```text
idf.py build  -> 成功
tab5book.bin  -> 2.56 MB
应用分区      -> 8 MB（剩余约 70%）
DIRAM         -> 378,622 B / 65.68%
内置字体      -> 约 1.39 MB Flash
```

## 烧录

```powershell
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.6'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:PATH = "D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;$env:PATH"
. "$env:IDF_PATH\export.ps1"

cd D:\work_dsh\tab5book
idf.py -p COMx flash monitor
```

生成固件：[build/tab5book.bin](build/tab5book.bin)

## 测试书籍

`samples/` 内已生成可直接复制到 SD 卡的测试文件：

- [荷塘月色.txt](samples/荷塘月色.txt)（UTF-8）
- [荷塘月色_GBK.txt](samples/荷塘月色_GBK.txt)（GBK 编码）
- [示例书籍.epub](samples/示例书籍.epub)（两章 + NCX 目录）
- [示例文档.pdf](samples/示例文档.pdf)（中文 Type0/ToUnicode 页 + 英文 Helvetica 页）

开机后进入“打开 SD 卡”，即可浏览并打开这些书籍。

## 重要限制

- **PDF 是文本模式**：提取文字层后重新排版，不是完整页面光栅化。
  - 文字版 PDF 可读
  - 扫描版/纯图片 PDF 会提示“没有可提取的文字”
  - 加密 PDF 不支持
- **EPUB 只提取正文文字**：不显示图片、不执行 CSS，不支持 DRM。
- 字体为 GB2312 子集，极少数生僻字/繁体字可能缺字。
- TXT 上限 8 MB，PDF 上限 16 MB。

## 说明

- 完整中文构建、使用、目录结构和故障排查见 [README.md](README.md)。
- 当前机器未检测到可用串口，因此只完成了编译验证，未做实机运行验证。
- 项目使用 8 MB 应用分区；如后续裁剪或扩大字体，可修改 [partitions.csv](partitions.csv) 和 [sdkconfig.defaults](sdkconfig.defaults)。
