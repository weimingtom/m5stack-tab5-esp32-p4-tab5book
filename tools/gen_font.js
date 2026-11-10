#!/usr/bin/env node
/*
 * Wrapper around lv_font_conv that feeds the (large) character set through
 * process.argv directly, avoiding the Windows 8 KB command-line limit.
 *
 *   set FONT_PATH=C:\Windows\Fonts\NotoSansSC-VF.ttf
 *   set LV_FONT_CONV=C:\path\to\lv_font_conv.js
 *   node tools/gen_font.js
 */
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const charset = fs
  .readFileSync(path.join(root, 'tools', 'charset.txt'), 'utf8')
  .replace(/\s+/g, '');

const converter =
  process.env.LV_FONT_CONV ||
  path.join(process.env.TEMP || '.', 'fonttool', 'node_modules', 'lv_font_conv', 'lv_font_conv.js');
const fontPath = process.env.FONT_PATH || 'C:\\Windows\\Fonts\\NotoSansSC-VF.ttf';
const size = process.env.FONT_SIZE || '24';
const name = process.env.FONT_NAME || 'cjk_font_' + size;
const output = path.join(root, 'components', 'book_font', name + '.c');

process.argv = [
  process.argv[0],
  __filename,
  '--font', fontPath,
  '--size', size,
  '--bpp', '4',
  '--format', 'lvgl',
  '--lv-include', 'lvgl.h',
  '--lv-font-name', name,
  '--symbols', charset,
  '--force-fast-kern-format',
  '--output', output,
];

console.log(`generating ${name}: ${charset.length} characters -> ${output}`);
require(converter);
