# -*- coding: utf-8 -*-
"""切换 beauty.ini 里的 dump_enable（诊断导出开关）。

为什么需要这个脚本：1.1.3.0 的 dump 通道只认环境变量 BEAUTY_DUMP_OUT，而
Resolve 从桌面图标启动时 bat 里 set 的变量继承不到 —— 1.1.4.0 那次验证就是
这样空手而归的。1.1.5.0 起开关落到 beauty.ini，本脚本负责 on/off 切换，
让 bat 不必自己处理 UTF-8 BOM 与 CRLF。

用法：
    python dump_switch.py on       # dump_enable = 1
    python dump_switch.py off      # dump_enable = 0
    python dump_switch.py status   # 只报告当前值

退出码：0 成功，2 参数错或找不到 ini。

事实来源：ini 路径与解析规则见 01_ofx插件源码/src/beauty_hwprobe.cpp
（cfgPathRef 拼 %ProgramData%\\DaVinciBeauty\\beauty.ini；cfgFind 取最后一次
出现的同名键）。
"""

import os
import re
import sys

KEY = 'dump_enable'
LINE_RE = re.compile(r'^\s*' + KEY + r'\s*=', re.IGNORECASE)
VALUE_RE = re.compile(r'^\s*' + KEY + r'\s*=\s*(.*?)\s*$', re.IGNORECASE)


def ini_path():
    pd = os.environ.get('ProgramData') or r'C:\ProgramData'
    return os.path.join(pd, 'DaVinciBeauty', 'beauty.ini')


def load(path):
    """返回 (行列表, 是否带 BOM)。UTF-8 严格解码，失败则按 gbk 兜底。"""
    raw = open(path, 'rb').read()
    bom = raw.startswith(b'\xef\xbb\xbf')
    body = raw[3:] if bom else raw
    try:
        text = body.decode('utf-8')
    except UnicodeDecodeError:
        text = body.decode('gbk', 'replace')
    return text.splitlines(), bom


def save(path, lines, bom):
    out = '\r\n'.join(lines) + '\r\n'
    data = (b'\xef\xbb\xbf' if bom else b'') + out.encode('utf-8')
    with open(path, 'wb') as f:
        f.write(data)


def main():
    action = (sys.argv[1] if len(sys.argv) > 1 else 'status').strip().lower()
    if action not in ('on', 'off', 'status'):
        print('[ERR] 用法: dump_switch.py on|off|status')
        return 2

    path = ini_path()
    if not os.path.exists(path):
        print('[ERR] 找不到配置文件: %s' % path)
        print('      （先让 Resolve 至少跑一次，插件会自动生成模板）')
        return 2

    lines, bom = load(path)
    hits = [i for i, l in enumerate(lines) if LINE_RE.match(l)]

    current = None
    if hits:
        m = VALUE_RE.match(lines[hits[-1]])
        current = m.group(1) if m else ''

    if action == 'status':
        shown = current if current is not None else '(文件中未出现该键，按默认 0 处理)'
        print('[%s] %s = %s' % (path, KEY, shown))
        return 0

    want = '1' if action == 'on' else '0'

    if hits:
        # 只改最后一条（解析器也是取最后一条），并清掉前面重复的旧键，
        # 免得文件越长越乱。
        lines[hits[-1]] = '%s = %s' % (KEY, want)
        for i in reversed(hits[:-1]):
            del lines[i]
    else:
        lines.append('%s = %s' % (KEY, want))

    save(path, lines, bom)
    print('[OK] %s = %s  -> %s' % (KEY, want, path))
    if want == '1':
        print('     记得改回 0（本脚本 off，或手动编辑），否则会一直往盘上写图。')
    return 0


if __name__ == '__main__':
    sys.exit(main())
