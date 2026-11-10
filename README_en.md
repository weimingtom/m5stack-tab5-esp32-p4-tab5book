# Tab5 Book Reader

A local ebook reader for the **M5Stack Tab5 (ESP32-P4)**, built on **ESP-IDF 5.5.6 + LVGL 9.6 + the M5Stack Tab5 BSP**.

Browse and read books directly from an SD card:

| Format | Support | Details |
| --- | --- | --- |
| **TXT** | Full | UTF-8 / UTF-16 (BOM) / GBK / two-byte GB18030; automatic pagination |
| **EPUB** | Text mode | ZIP extraction, OPF/Spine parsing, NCX table of contents, XHTML-to-text, chapter navigation |
| **PDF** | Text mode | Parses xref tables/streams, object streams, fonts and ToUnicode; extracts text page by page and reflows it |

> PDF support uses **text-layer extraction + reflow**, not full rasterization.
> PDFs with a text layer (most ebooks and office documents) are readable.
> Scanned or image-only PDFs have no text layer and will show a notice instead.
> Encrypted PDFs are not supported.
>
> EPUB support currently extracts body text only; images are not displayed and
> CSS is not applied. DRM-protected EPUBs are not supported.

---

## Features

- 5-inch 1280×720 landscape reading UI with a 24 px CJK bitmap font (full GB2312 set plus common symbols)
- SD-card file browser: enter/leave directories, refresh, directories sorted first, file type and size shown
- Reader:
  - Tap the left/right side to turn pages, tap the center to show/hide the toolbars, swipe left/right to turn pages
  - Top bar: back, book title, format badge, theme, table of contents/jump
  - Bottom bar: page number, chapter title, reading progress bar, previous/next page
  - EPUB: chapter list
  - PDF: jump by PDF page number
  - TXT: jump by percentage of the whole book
- Three themes: day / eye-care / night; screen brightness adjustment
- Reading progress and recent books saved in NVS; resume after reboot
- Recent-books list (up to 8 entries)
- CJK font compiled into the firmware; no font file needed on the SD card

---

## Hardware and software

- **Board**: M5Stack Tab5 (ESP32-P4, 16 MB flash, 32 MB PSRAM, 720×1280 MIPI-DSI display, GT911 touch)
- **ESP-IDF**: v5.5.x (this project was verified with **v5.5.6**)
- **Display orientation**: rotated 90° at startup, so the UI runs at 1280×720 landscape
- **Storage**: microSD card (FAT32/exFAT; FAT32 recommended)

---

## Quick start

### 1. Set up the ESP-IDF environment

The paths below are examples from this machine:

```powershell
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.6'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:PATH = "D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;D:\Espressif\tools\idf-git\2.44.0\cmd;$env:PATH"
. "$env:IDF_PATH\export.ps1"

cd D:\work_dsh\tab5book
```

You can also run `tools\idf_shell.bat` to open a preconfigured ESP-IDF command prompt.

### 2. Build

```powershell
idf.py set-target esp32p4   # only for the first build
idf.py build
```

### 3. Flash and view logs

```powershell
idf.py -p COMx flash monitor
```

The Tab5 uses the USB-C USB-Serial/JTAG port for flashing and logs.
`sdkconfig.defaults` already configures the console for USB-Serial/JTAG.

### 4. Copy books to the SD card

Copy books to the SD-card root or any subdirectory, for example:

```
SD card root/
├─ books/
│  ├─ Three-Body.epub
│  ├─ sample-document.pdf
│  └─ moon-over-the-lotus-pond.txt
└─ other/
   └─ ...
```

At boot, open the desktop → **Open SD Card** → browse to a book → tap to open it.

The `samples/` directory contains four test files that can be copied directly to the SD card:

- `荷塘月色.txt` (UTF-8)
- `荷塘月色_GBK.txt` (GBK encoding test)
- `示例书籍.epub` (two chapters + NCX TOC)
- `示例文档.pdf` (Chinese Type0/ToUnicode page + English Helvetica page)

---

## Controls

### Desktop

- **Open SD Card**: open the file browser
- **Continue Reading**: open the last book
- **Refresh Recent**: rebuild the desktop so the recent-books panel reflects the latest NVS data
- **Settings**: theme, brightness, clear reading history
- **About**: version and format information

### File browser

- **Home**: return to the desktop
- **Up**: go to the parent directory
- **Refresh**: rescan the current directory
- Tap a directory to enter it; tap a `.txt` / `.md` / `.epub` / `.pdf` file to open it

### Reader

| Action | Effect |
| --- | --- |
| Tap the left 240 px of the screen | Previous page |
| Tap the right 240 px of the screen | Next page |
| Tap the center of the screen | Show/hide the top and bottom toolbars |
| Swipe left/right | Previous/next page |
| Bottom **Previous/Next** buttons | Turn pages |
| Top **Contents** | EPUB chapter list |
| Top **Jump** | PDF page number / TXT percentage |
| Top **Theme** | Cycle day → eye-care → night |
| Top **Back** | Save progress and return to the file browser |

---

## Project structure

```
tab5book/
├─ CMakeLists.txt
├─ partitions.csv                 # 8 MB application partition
├─ sdkconfig.defaults             # Tab5 / PSRAM / USB console / LVGL configuration
├─ main/
│  ├─ app_main.c                  # Display, touch, SD, UI startup
│  ├─ idf_component.yml           # m5stack_tab5 BSP + zlib
│  ├─ reader/
│  │  ├─ doc.c / doc.h            # Unified document interface
│  │  ├─ text.c / text.h          # UTF-8/UTF-16/GBK decoding, CJK line breaking and pagination
│  │  ├─ txt.c                    # TXT document
│  │  ├─ zip.c / zip.h            # Minimal ZIP reader (stored/deflate)
│  │  ├─ epub.c                   # EPUB container/OPF/NCX/XHTML
│  │  ├─ pdf.c                    # PDF objects/xref/streams/fonts/ToUnicode
│  │  ├─ pdf_text.c               # PDF content-stream text extraction and reflow
│  │  └─ gbk_table.h              # GBK→Unicode table (generated)
│  ├─ storage/
│  │  └─ library.c                # NVS recent books/progress/theme/brightness
│  └─ ui/
│     ├─ ui.c                     # Desktop, file browser, settings, dialogs
│     └─ ui_reader.c              # Reader UI, contents, jump
├─ components/book_font/
│  ├─ cjk_font_24.c               # 24 px CJK LVGL font (generated)
│  └─ book_font.c                 # Font wrapper with adjusted line heights
├─ samples/                       # Test books
└─ tools/
   ├─ gen_charset.py              # Generate character set + GBK table
   ├─ gen_font.js                 # Invoke lv_font_conv to generate the font C file
   ├─ gen_pdf_tables.py           # Generate PDF encoding/glyph-name tables
   └─ make_samples.py             # Generate test TXT/EPUB/PDF files
```

---

## Font and character set

`components/book_font/cjk_font_24.c` is generated from **Noto Sans SC** at 24 px, 4 bpp,
and covers:

- ASCII / Latin-1
- All GB2312 ideographs (6763 characters)
- Common punctuation, full-width symbols, arrows, geometric shapes, and some names/rare characters

Generation steps:

```bash
# 1) Character set and GBK table
python tools/gen_charset.py

# 2) Install lv_font_conv (Node.js)
npm install lv_font_conv@1.5.3

# 3) Generate the font (the wrapper avoids Windows command-line length limits)
set FONT_PATH=C:\Windows\Fonts\NotoSansSC-VF.ttf
set LV_FONT_CONV=C:\path\to\node_modules\lv_font_conv\lv_font_conv.js
node tools/gen_font.js
```

The compiled font object is about 1.4 MB (RLE-compressed) and occupies part of the
application partition. The current application partition is 8 MB; adjust `partitions.csv`
if you need a larger font or more glyphs.

Font license: Noto Sans SC is licensed under the SIL Open Font License 1.1.

---

## PDF reading notes

This is a lightweight PDF text reader for embedded devices:

- Supports xref tables / xref streams / object streams (ObjStm)
- Supports FlateDecode (including PNG/TIFF predictors), ASCIIHex, ASCII85, and RunLength
- Supports Type0 (Identity-H) / Type1 / TrueType fonts
- Prefers `/ToUnicode` CMaps; simple fonts fall back to WinAnsi/MacRoman/Standard encodings and `/Differences`
- Supports `BT/ET`, `Tf`, `Tm`, `Td`, `TD`, `T*`, `Tj`, `TJ`, `'`, `"`, `TL`, `Tc`, `Tw`, `Tz`, `Ts`, `cm`, and `q/Q`
- Does not support: encrypted PDFs, scanned image pages, embedded image rendering, or full vector graphics rendering

Consequently:

- Text-based PDFs: readable, reflowed to the reader width while preserving paragraph/line spacing
- Scanned PDFs: show “No extractable text on this page; this may be a scanned PDF”
- Uncommon PDFs with non-embedded fonts and no ToUnicode may lose some text

---

## Known limitations

- TXT files are limited to 8 MB; PDF files to 16 MB (limited by PSRAM and parsing time)
- EPUB does not support DRM, images, or CSS layout; only text is extracted
- The font covers GB2312; a very small number of rare or traditional characters may be missing
- The table of contents shows at most the first 150 chapters
- Reading progress is stored as “chapter + page”; changing the theme does not change pagination, and progress can be restored after reboot
- Screen rotation uses the BSP's software rotation; if a future BSP supports hardware rotation, `sw_rotate` can be disabled

---

## Build verification

The project has been verified with `idf.py build` (ESP-IDF v5.5.6, target `esp32p4`).

Results from this machine:

| Item | Value |
| --- | --- |
| `build/tab5book.bin` | about **2.56 MB** |
| Application partition | 8 MB (about 70% free) |
| DIRAM static usage | 378,622 B / 65.68%, about 193 KB free |
| Built-in CJK font | about 1.39 MB (Flash `.rodata`) |
| LVGL | about 651 KB |

Inspect detailed usage with:

```powershell
idf.py size
idf.py size-components
```

If the application exceeds 8 MB, you can:

1. Increase the `factory` partition in `partitions.csv` (the flash has 16 MB total; 8 MB is currently used)
2. Reduce the font size or character set (`tools/gen_charset.py` + `tools/gen_font.js`)
3. Disable unused LVGL libraries (`LV_USE_*` in `sdkconfig`)

---

## Troubleshooting

| Symptom | What to do |
| --- | --- |
| Black screen | Confirm the target is `esp32p4`; check the BSP initialization logs; check backlight/power |
| Touch direction is wrong | Confirm `bsp_display_rotate(..., LV_DISPLAY_ROTATION_90)` is called; the BSP synchronizes touch coordinates automatically |
| SD card cannot be mounted | Use FAT32; reinsert the card; check the `SD card mount` log over serial |
| Chinese text appears as boxes | Confirm `components/book_font/cjk_font_24.c` is compiled in; regenerate the font |
| PDF fails to open | It may be encrypted or too large; the serial log prints the specific error |
| EPUB fails to open | It may be ZIP64/DRM/corrupted; try re-exporting with Calibre |
| Font compilation is slow during build | The font C file is about 8.7 MB; the first build takes a while, and incremental builds reuse the object file |

---

## License

- Project code: MIT (modify as needed)
- Noto Sans SC font: SIL Open Font License 1.1
- Sample text “Moon over the Lotus Pond” by Zhu Ziqing: public domain
- Sample EPUB/PDF: generated by `tools/make_samples.py`; free to use
