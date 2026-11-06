# -*- coding: utf-8 -*-
"""
BUG4 分叉测试（gpu_smooth = off）—— 只跑用例 C：V1、V2 都挂美颜。

用法：
    python bug4_caseC.py probe     # 只读：报告工程 / 轨道 / 效果 / 播放头
    python bug4_caseC.py setup     # 确保双轨都有美颜（缺则补）
    python bug4_caseC.py shots     # 依次抓 4 张：B1 B2 C1 C2
    python bug4_caseC.py cmp       # 像素比对并出结论
    python bug4_caseC.py all       # setup + shots + cmp

抓图方法沿用 12:45 的做法：先播放 1.6 秒强制重绘，暂停后再截 Resolve 窗口。
每张图额外补一张「回到固定帧 F 再截」的图，用于排除播放时机抖动。
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import sys
import time

import numpy as np

sys.path.append(r'C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting\Modules')
os.environ.setdefault('RESOLVE_SCRIPT_LIB', r'D:\DaVinci Resolve\fusionscript.dll')

import DaVinciResolveScript as dvr  # noqa: E402

FIXTURE_DIR = os.path.dirname(os.path.abspath(__file__))
SHOT_DIR = os.path.join(FIXTURE_DIR, 'bug4_shots')
STATE_JSON = os.path.join(FIXTURE_DIR, 'bug4_caseC_state.json')
BEAUTY = 'Open FX/DaVinci Beauty/DaVinci Beauty 美颜'
FIXED_FRAME = 300          # 固定对拍帧
PLAY_SECONDS = 1.6


# ---------------------------------------------------------------- Resolve 侧
def conn():
    r = dvr.scriptapp('Resolve')
    if r is None:
        raise SystemExit('连接 Resolve 失败：请确认 Resolve 正在运行')
    return r


def ctx():
    r = conn()
    p = r.GetProjectManager().GetCurrentProject()
    return r, p, p.GetCurrentTimeline()


def report():
    r, p, tl = ctx()
    print('project :', p.GetName())
    print('timeline:', tl.GetName(), 'fps=', p.GetSetting('timelineFrameRate'),
          'res=', p.GetSetting('timelineResolutionWidth'), 'x', p.GetSetting('timelineResolutionHeight'))
    print('playhead:', tl.GetCurrentTimecode(), 'range=', tl.GetStartFrame(), '-', tl.GetEndFrame())
    for i in (1, 2):
        items = tl.GetItemListInTrack('video', i) or []
        print('V%d enabled=%s items=%d' % (i, tl.GetIsTrackEnabled('video', i), len(items)))
        for it in items:
            fx = [(f.GetName(), f.GetIsEnabled()) for f in (it.GetEffects() or [])]
            print('   ', it.GetName()[:38], '| fx:', fx)


def ensure_beauty():
    """确保 V1、V2 各有一个美颜效果且启用；返回补了几个。"""
    r, p, tl = ctx()
    added = 0
    for i in (1, 2):
        for it in (tl.GetItemListInTrack('video', i) or []):
            fx = it.GetEffects() or []
            names = [f.GetName() for f in fx]
            if not any('美颜' in n or 'Beauty' in n for n in names):
                ok = it.AddEffect(BEAUTY)
                print('V%d 补挂美颜 -> %s' % (i, ok))
                added += 1
                fx = it.GetEffects() or []
            for f in fx:
                if '美颜' in f.GetName() or 'Beauty' in f.GetName():
                    if f.GetIsEnabled() is False:
                        f.SetIsEnabled(True)
                        print('V%d 美颜重新启用' % i)
    return added


def beauty_effects():
    r, p, tl = ctx()
    out = {}
    for i in (1, 2):
        for it in (tl.GetItemListInTrack('video', i) or []):
            for f in (it.GetEffects() or []):
                if '美颜' in f.GetName() or 'Beauty' in f.GetName():
                    out[i] = f
    return out


def set_beauty(enabled):
    for i, f in beauty_effects().items():
        f.SetIsEnabled(bool(enabled))
    time.sleep(0.6)


def set_track(track, enabled):
    r, p, tl = ctx()
    tl.SetTrackEnable('video', track, bool(enabled))
    time.sleep(1.0)


# ---------------------------------------------------------------- 窗口与截图
user32 = ctypes.windll.user32
try:
    ctypes.windll.shcore.SetProcessDpiAwareness(2)
except Exception:
    pass


def find_resolve_hwnd():
    hwnds = []

    @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
    def cb(hwnd, _):
        if not user32.IsWindowVisible(hwnd):
            return True
        n = user32.GetWindowTextLengthW(hwnd)
        if n:
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if 'DaVinci Resolve' in buf.value:
                hwnds.append((hwnd, buf.value))
        return True

    user32.EnumWindows(cb, 0)
    return hwnds[0][0] if hwnds else None


def window_rect(hwnd):
    rect = wt.RECT()
    # 优先拿 DWM 扩展边框，避免 Win10 阴影边
    try:
        DWMWA_EXTENDED_FRAME_BOUNDS = 9
        margin = wt.RECT()
        ctypes.windll.dwmapi.DwmGetWindowAttribute(
            wt.HWND(hwnd), DWMWA_EXTENDED_FRAME_BOUNDS, ctypes.byref(margin), ctypes.sizeof(margin))
        if margin.right > margin.left and margin.bottom > margin.top:
            return margin.left, margin.top, margin.right, margin.bottom
    except Exception:
        pass
    user32.GetWindowRect(wt.HWND(hwnd), ctypes.byref(rect))
    return rect.left, rect.top, rect.right, rect.bottom


def focus(hwnd):
    try:
        user32.ShowWindow(wt.HWND(hwnd), 9)
        user32.SetForegroundWindow(wt.HWND(hwnd))
    except Exception:
        pass
    time.sleep(0.3)


def key_space():
    VK_SPACE = 0x20
    user32.keybd_event(VK_SPACE, 0, 0, 0)
    time.sleep(0.02)
    user32.keybd_event(VK_SPACE, 0, 2, 0)


def grab(hwnd, tag):
    from PIL import ImageGrab
    os.makedirs(SHOT_DIR, exist_ok=True)
    l, t, rr, b = window_rect(hwnd)
    img = ImageGrab.grab(bbox=(l, t, rr, b), all_screens=True)
    path = os.path.join(SHOT_DIR, tag + '.png')
    img.save(path)
    return path


def shot(tag, track2_enabled):
    """一次取样：设定 V2 开关 → 播放 1.6s → 暂停截图 → 归位固定帧再截一张。"""
    hwnd = find_resolve_hwnd()
    if not hwnd:
        raise SystemExit('找不到 DaVinci Resolve 主窗口')
    r, p, tl = ctx()
    set_track(2, track2_enabled)
    focus(hwnd)
    tl.SetCurrentTimecode(tc_of(FIXED_FRAME))
    time.sleep(0.4)
    key_space()                     # 播放
    time.sleep(PLAY_SECONDS)
    key_space()                     # 暂停
    time.sleep(0.5)
    a = grab(hwnd, tag + '_pause')
    tl.SetCurrentTimecode(tc_of(FIXED_FRAME))
    time.sleep(0.8)
    b = grab(hwnd, tag + '_seek')
    print('[shot] %-4s V2=%s -> %s / %s' % (tag, track2_enabled, os.path.basename(a), os.path.basename(b)))
    return a, b


def tc_of(frame):
    r, p, tl = ctx()
    fps = float(p.GetSetting('timelineFrameRate').split()[0])
    fps = round(fps)
    s, f = divmod(int(frame), fps)
    m, s = divmod(s, 60)
    h, m = divmod(m, 60)
    return '%02d:%02d:%02d:%02d' % (h, m, s, f)


# ---------------------------------------------------------------- 比对
def cmp_images(path_a, path_b):
    from PIL import Image
    a = np.asarray(Image.open(path_a).convert('RGB'), dtype=np.int16)
    b = np.asarray(Image.open(path_b).convert('RGB'), dtype=np.int16)
    h = min(a.shape[0], b.shape[0]); w = min(a.shape[1], b.shape[1])
    # 裁掉窗口边框：只取中间 viewer 区域（上下各去 12%，左右各去 18%）
    a = a[int(h * .12):int(h * .88), int(w * .18):int(w * .82)]
    b = b[int(h * .12):int(h * .88), int(w * .18):int(w * .82)]
    d = np.abs(a - b).max(axis=2)
    return float((d > 8).mean() * 100.0)


def do_cmp():
    want = ['B1', 'B2', 'C1', 'C2']
    paths = {}
    for t in want:
        for suf in ('_pause', '_seek'):
            p = os.path.join(SHOT_DIR, t + suf + '.png')
            paths[t + suf] = p if os.path.exists(p) else None
    print('\n=== 像素差异（% 差异像素，阈值>8）===')
    for suf in ('_pause', '_seek'):
        print('\n[%s]' % suf.strip('_'))
        for t in want:
            if not paths[t + suf]:
                print('  %s 缺失' % (t + suf)); continue
            for base in ('B1', 'B2'):
                if not paths[base + suf]:
                    continue
                print('  %s vs %s : %6.2f%%' % (t, base, cmp_images(paths[t + suf], paths[base + suf])))
    print('\n判读：C1 与 B1（V2 层）差异小 → 显示正确；与 B2（V1 层）差异小 → 塌陷到 V1（BUG 复现）')


def main():
    step = (sys.argv[1] if len(sys.argv) > 1 else 'probe').lower()
    if step == 'probe':
        report()
    elif step == 'setup':
        report()
        ensure_beauty()
        print('--- 补挂后 ---')
        report()
    elif step == 'shots':
        set_beauty(False)      # 基准：美颜全关
        shot('B1', True)       # 应显示 V2
        shot('B2', False)      # 应显示 V1
        set_beauty(True)       # 用例 C：两轨美颜
        shot('C1', True)       # 应显示 V2
        shot('C2', False)      # 应显示 V1
    elif step == 'cmp':
        do_cmp()
    elif step == 'all':
        report()
        ensure_beauty()
        set_beauty(False)
        shot('B1', True)
        shot('B2', False)
        set_beauty(True)
        shot('C1', True)
        shot('C2', False)
        do_cmp()
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
