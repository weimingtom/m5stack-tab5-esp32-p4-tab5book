#!/usr/bin/env python3
"""Create small demo books for the Tab5 reader.

    python tools/make_samples.py

Writes UTF-8/GBK text, a two-chapter EPUB and a two-page PDF (one page uses a
Type0/ToUnicode font, the other a base-14 Helvetica font) under samples/.
"""
from __future__ import annotations

import os
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "samples")

TEXT = """荷塘月色（节选）

这几天心里颇不宁静。今晚在院子里坐着乘凉，忽然想起日日走过的荷塘，在这满月的光里，总该另有一番样子吧。月亮渐渐地升高了，墙外马路上孩子们的欢笑，已经听不见了；妻在屋里拍着闰儿，迷迷糊糊地哼着眠歌。我悄悄地披了大衫，带上门出去。

沿着荷塘，是一条曲折的小煤屑路。这是一条幽僻的路；白天也少人走，夜晚更加寂寞。荷塘四面，长着许多树，蓊蓊郁郁的。路的一旁，是些杨柳，和一些不知道名字的树。没有月光的晚上，这路上阴森森的，有些怕人。今晚却很好，虽然月光也还是淡淡的。

曲曲折折的荷塘上面，弥望的是田田的叶子。叶子出水很高，像亭亭的舞女的裙。层层的叶子中间，零星地点缀着些白花，有袅娜地开着的，有羞涩地打着朵儿的；正如一粒粒的明珠，又如碧天里的星星，又如刚出浴的美人。微风过处，送来缕缕清香，仿佛远处高楼上渺茫的歌声似的。

月光如流水一般，静静地泻在这一片叶子和花上。薄薄的青雾浮起在荷塘里。叶子和花仿佛在牛乳中洗过一样；又像笼着轻纱的梦。虽然是满月，天上却有一层淡淡的云，所以不能朗照；但我以为这恰是到了好处——酣眠固不可少，小睡也别有风味的。

（完）
"""


def write_txt() -> None:
    path = os.path.join(OUT, "荷塘月色.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(TEXT)
    gbk = os.path.join(OUT, "荷塘月色_GBK.txt")
    with open(gbk, "wb") as f:
        f.write(TEXT.encode("gbk"))


def write_epub() -> None:
    path = os.path.join(OUT, "示例书籍.epub")
    chapters = [
        (
            "chapter1.xhtml",
            "第一章 荷塘",
            "这几天心里颇不宁静。今晚在院子里坐着乘凉，忽然想起日日走过的荷塘。",
        ),
        (
            "chapter2.xhtml",
            "第二章 月色",
            "月光如流水一般，静静地泻在这一片叶子和花上。薄薄的青雾浮起在荷塘里。",
        ),
    ]
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(
            zipfile.ZipInfo("mimetype"),
            "application/epub+zip",
            compress_type=zipfile.ZIP_STORED,
        )
        z.writestr(
            "META-INF/container.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
""",
            compress_type=zipfile.ZIP_DEFLATED,
        )
        z.writestr(
            "OEBPS/content.opf",
            """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" unique-identifier="bookid" xmlns="http://www.idpf.org/2007/opf">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>示例书籍</dc:title>
    <dc:creator>Tab5 Reader</dc:creator>
    <dc:identifier id="bookid">urn:uuid:tab5book-demo-0001</dc:identifier>
    <dc:language>zh-CN</dc:language>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="ch1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="ch2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="ch1"/>
    <itemref idref="ch2"/>
  </spine>
</package>
""",
            compress_type=zipfile.ZIP_DEFLATED,
        )
        z.writestr(
            "OEBPS/toc.ncx",
            """<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head><meta name="dtb:uid" content="tab5book-demo-0001"/></head>
  <docTitle><text>示例书籍</text></docTitle>
  <navMap>
    <navPoint id="n1" playOrder="1">
      <navLabel><text>第一章 荷塘</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
    <navPoint id="n2" playOrder="2">
      <navLabel><text>第二章 月色</text></navLabel>
      <content src="chapter2.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
""",
            compress_type=zipfile.ZIP_DEFLATED,
        )
        for name, title, body in chapters:
            z.writestr(
                "OEBPS/" + name,
                f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{title}</title><style>p {{ margin: 1em 0; }}</style></head>
<body>
<h1>{title}</h1>
<p>{body}</p>
<p>这是一段用于验证 EPUB 解压、OPF 解析、NCX 目录和中文排版的示例文字。</p>
</body>
</html>
""",
                compress_type=zipfile.ZIP_DEFLATED,
            )


def esc(s: str) -> str:
    return s.replace("\\", "\\\\").replace("(", "\\(").replace(")", "\\)")


def make_pdf() -> None:
    path = os.path.join(OUT, "示例文档.pdf")

    lines = [
        "中文 PDF 文本层测试",
        "本页使用 Type0 / Identity-H 字体和 ToUnicode CMap。",
        "阅读器应当逐页提取文字并重新排版。",
        "第二页使用 Helvetica 标准字体，用于测试简单字体路径。",
    ]
    # Assign a two-byte code to every character used on page one.
    chars: list[str] = []
    code: dict[str, int] = {}
    for line in lines:
        for ch in line:
            if ch not in code:
                code[ch] = len(chars) + 1
                chars.append(ch)

    def hex_line(line: str) -> str:
        return "".join(f"{code[ch]:04X}" for ch in line)

    content1_lines = ["BT", "/F1 22 Tf", "1 0 0 1 60 770 Tm", f"<{hex_line(lines[0])}> Tj"]
    y = 730
    content1_lines += ["/F1 15 Tf"]
    for line in lines[1:]:
        content1_lines += [f"1 0 0 1 60 {y} Tm", f"<{hex_line(line)}> Tj"]
        y -= 26
    content1_lines.append("ET")
    content1 = "\n".join(content1_lines).encode("latin-1")

    content2 = (
        "BT\n/F2 24 Tf\n1 0 0 1 60 720 Tm\n(Hello Tab5 PDF reader) Tj\n"
        "0 -36 Td\n/F2 14 Tf\n(This page uses the standard Helvetica font.) Tj\nET\n"
    ).encode("latin-1")

    bfchar = "\n".join(f"<{code[ch]:04X}> <{ord(ch):04X}>" for ch in chars)
    tounicode = f"""/CIDInit /ProcSet findresource begin
12 dict begin
begincmap
/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def
/CMapName /Adobe-Identity-UCS def
/CMapType 2 def
1 begincodespacerange
<0000> <FFFF>
endcodespacerange
{len(chars)} beginbfchar
{bfchar}
endbfchar
endcmap
CMapName currentdict /CMap defineresource pop
end
end
""".encode("latin-1")

    objects: list[bytes] = []
    # 1 catalog, 2 pages, 3/4 page objects, 5 Type0 font, 6 descendant,
    # 7 ToUnicode, 8 descriptor, 9/10 contents, 11 Helvetica.
    objects.append(b"<< /Type /Catalog /Pages 2 0 R >>")
    objects.append(b"<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>")
    objects.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 9 0 R >>"
    )
    objects.append(
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] "
        b"/Resources << /Font << /F2 11 0 R >> >> /Contents 10 0 R >>"
    )
    objects.append(
        b"<< /Type /Font /Subtype /Type0 /BaseFont /NotoSansSC "
        b"/Encoding /Identity-H /DescendantFonts [6 0 R] /ToUnicode 7 0 R >>"
    )
    objects.append(
        b"<< /Type /Font /Subtype /CIDFontType2 /BaseFont /NotoSansSC "
        b"/CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> "
        b"/FontDescriptor 8 0 R /DW 1000 >>"
    )
    objects.append(
        b"<< /Length " + str(len(tounicode)).encode() + b" >>\nstream\n" + tounicode + b"\nendstream"
    )
    objects.append(
        b"<< /Type /FontDescriptor /FontName /NotoSansSC /Flags 4 "
        b"/FontBBox [0 -200 1000 900] /ItalicAngle 0 /Ascent 900 /Descent -200 "
        b"/CapHeight 700 /StemV 80 >>"
    )
    objects.append(
        b"<< /Length " + str(len(content1)).encode() + b" >>\nstream\n" + content1 + b"\nendstream"
    )
    objects.append(
        b"<< /Length " + str(len(content2)).encode() + b" >>\nstream\n" + content2 + b"\nendstream"
    )
    objects.append(
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>"
    )

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets: list[int] = []
    for i, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += f"{i} 0 obj\n".encode() + body + b"\nendobj\n"
    xref = len(out)
    out += f"xref\n0 {len(objects) + 1}\n".encode()
    out += b"0000000000 65535 f \n"
    for off in offsets:
        out += f"{off:010d} 00000 n \n".encode()
    out += (
        f"trailer\n<< /Size {len(objects) + 1} /Root 1 0 R >>\nstartxref\n{xref}\n%%EOF\n"
    ).encode()
    with open(path, "wb") as f:
        f.write(out)


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    write_txt()
    write_epub()
    make_pdf()
    print(f"samples written to {OUT}")


if __name__ == "__main__":
    main()
