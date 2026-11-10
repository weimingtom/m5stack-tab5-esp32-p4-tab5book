# 测试书籍

把本目录的文件复制到 SD 卡（任意目录）即可在 Tab5 阅读器中打开。

| 文件 | 用途 |
| --- | --- |
| `荷塘月色.txt` | UTF-8 中文 TXT，测试分页、排版、进度 |
| `荷塘月色_GBK.txt` | GBK 编码，测试自动编码检测 |
| `示例书籍.epub` | 两章 EPUB，测试 ZIP/OPF/NCX 目录/章节跳转 |
| `示例文档.pdf` | 两页 PDF：第 1 页中文 Type0+ToUnicode，第 2 页英文 Helvetica |

这些文件由 `tools/make_samples.py` 生成：

```bash
python tools/make_samples.py
```

PDF 示例只用于验证文本提取，不包含嵌入字体文件；在普通 PDF 阅读器中
第一页可能显示空白，但在本阅读器中会通过 ToUnicode 提取出中文文字。
