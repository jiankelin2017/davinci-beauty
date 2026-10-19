--[[
    NamaBeauty — DaVinci Resolve 集成脚本
    ==========================================
    皮肤 / 眼部美颜：磨皮、清晰、美白、红润、锐化、五官立体、亮眼、去黑眼圈

    架构（与 Qwen ASR 一致）：
      Lua 前端（达芬奇原生 UI）  →  Python 引擎（nama_worker.py）

      resolve:Fusion().UIManager
        → bmd.UIDispatcher(ui)
        → dispatcher:AddWindow{...}
        → dispatcher.On.Timeout + RunLoop

    为什么前端用 Lua 而不是 Python：
      Resolve 通用脚本目录下，Python 环境未必带 bmd 模块（本机实测
      "No module named 'bmd'"），而 Lua 环境里 bmd 是内置的。想要达芬奇
      原生窗口，只能走 Lua。

    异步约定：
      前端把 job 写成 JSON 文件，里面带上 progress / result 两个路径；
      worker 后台跑，边跑边覆写 progress，结束时落 result。
      前端用定时器轮询这两个文件，不阻塞消息泵。
]]

-- ══════════════════════════════════════════════════════════════════
-- 路径
-- ══════════════════════════════════════════════════════════════════
local TOOL_ROOT = "C:\\ProgramData\\NamaBeauty"        --<<TOOL_ROOT>>
local WORKER    = TOOL_ROOT .. "\\nama_worker.py"
local CONFIG    = TOOL_ROOT .. "\\config.json"
local JOBS      = TOOL_ROOT .. "\\jobs"
local LOGS      = TOOL_ROOT .. "\\logs"

-- 预览用独立的一套文件，避免和处理任务互相覆盖
local PREV_OUT  = LOGS .. "\\preview_compare.jpg"
local PREV_JOB  = JOBS .. "\\job_preview.json"
local PREV_PROG = LOGS .. "\\progress_preview.txt"
local PREV_RES  = LOGS .. "\\result_preview.json"

-- ── 实时预览 ──────────────────────────────────────────────────────
-- 单帧预览每次都要冷启动引擎（SDK 初始化 1.8 秒），所以快不了。
-- 实时预览改成「常驻进程」：nama_live.py 只启动一次，把 GL 上下文、
-- SDK、模型全部留在内存里，之后每帧只剩 渲染 10ms + 贴图 26ms。
-- 本脚本只负责把参数和播放头写进 live.json，并持续写 live.hb 当心跳；
-- 进程那边心跳一断就自行退出，不会留孤儿。
local LIVE_DIR    = TOOL_ROOT .. "\\live"
local LIVE_JSON   = LIVE_DIR .. "\\live.json"
local LIVE_HB     = LIVE_DIR .. "\\live.hb"
local LIVE_PID    = LIVE_DIR .. "\\live.pid"
local LIVE_QUIT   = LIVE_DIR .. "\\live.quit"
local LIVE_SCRIPT = TOOL_ROOT .. "\\nama_live.py"
local LIVE_TIMER  = "NamaBeautyLive"

-- (key, 中文标签, 默认刻度)
local EFFECTS = {
    { key = "blur_level",            label = "磨皮",     def = 70 },
    { key = "clarity",               label = "清晰",     def = 0  },
    { key = "color_level",           label = "美白",     def = 15 },
    { key = "red_level",             label = "红润",     def = 30 },
    { key = "sharpen",               label = "锐化",     def = 0  },
    { key = "face_threed",           label = "五官立体", def = 0  },
    { key = "eye_bright",            label = "亮眼",     def = 0  },
    { key = "remove_pouch_strength", label = "去黑眼圈", def = 0  },
}

-- (显示文本, 引擎取值)
local BLUR_TYPES = {
    { "双边滤波（最强·推荐）", 1 },
    { "高斯（中等）",          0 },
    { "弱磨皮",                2 },
}

local PRESETS = {
    { "H.264 / 显卡加速（NVENC）", "h264_nvenc" },
    { "H.264 / CPU 通用",          "libx264"    },
    { "Apple ProRes 422 HQ",       "prores422"  },
}

-- ══════════════════════════════════════════════════════════════════
-- 工具
-- ══════════════════════════════════════════════════════════════════
local function exists(p)
    if not p or p == "" then return false end
    if type(bmd) == "table" and type(bmd.fileexists) == "function" then
        local ok, r = pcall(bmd.fileexists, p)
        if ok and r then return true end
    end
    local f = io.open(p, "r")
    if f then f:close(); return true end
    return false
end

local function read_file(p)
    local f = io.open(p, "r")
    if not f then return "" end
    local s = f:read("*a")
    f:close()
    return s or ""
end

local function write_file(p, s)
    local f = io.open(p, "w")
    if not f then return false end
    f:write(s)
    f:close()
    return true
end

local function q(s)
    return '"' .. tostring(s) .. '"'
end

-- Lua 5.1 没有 table.unpack 别名
local unpack_ = table.unpack or unpack

-- 极简 JSON 编码器：只覆盖本脚本用到的结构
local function jval(v)
    local t = type(v)
    if t == "boolean" then
        return v and "true" or "false"
    elseif t == "number" then
        return tostring(v)
    elseif t == "string" then
        local s = v:gsub("\\", "\\\\"):gsub('"', '\\"'):gsub("\n", "\\n")
        return '"' .. s .. '"'
    elseif t == "table" then
        -- 先判断是不是数组
        local cnt, only_num = 0, true
        for k, _ in pairs(v) do
            cnt = cnt + 1
            if type(k) ~= "number" then only_num = false end
        end
        if cnt == 0 then return "{}" end
        local parts = {}
        if only_num then
            for i = 1, cnt do parts[#parts + 1] = jval(v[i]) end
            return "[" .. table.concat(parts, ",") .. "]"
        else
            for k, vv in pairs(v) do
                parts[#parts + 1] = '"' .. tostring(k) .. '":' .. jval(vv)
            end
            return "{" .. table.concat(parts, ",") .. "}"
        end
    end
    return '""'
end

-- 极简 JSON 解码器：只抓需要的字段，够用且不引入依赖
local function jget_num(text, key, dflt)
    local v = text:match('"' .. key .. '"%s*:%s*(-?%d+%.?%d*)')
    return tonumber(v) or dflt
end

local function jget_bool(text, key, dflt)
    local v = text:match('"' .. key .. '"%s*:%s*(%a+)')
    if v == "true"  then return true  end
    if v == "false" then return false end
    return dflt
end

local function jget_str(text, key, dflt)
    local v = text:match('"' .. key .. '"%s*:%s*"([^"]*)"')
    if v == nil or v == "" then return dflt end
    return v
end

-- ══════════════════════════════════════════════════════════════════
-- 时间线片段
-- ══════════════════════════════════════════════════════════════════
local function collect_clips(timeline)
    local out = {}
    local ok, n = pcall(function() return timeline:GetTrackCount("video") end)
    if not ok or not n then return out end
    for ti = 1, n do
        local items = timeline:GetItemListInTrack("video", ti)
        if items then
            for _, item in ipairs(items) do
                local mpi  = item:GetMediaPoolItem()
                local path = ""
                local name = ""
                if mpi then
                    local props = mpi:GetClipProperty()
                    if props then path = props["File Path"] or "" end
                    name = mpi:GetName() or ""
                end
                if path ~= "" and exists(path) then
                    out[#out + 1] = { item = item, path = path,
                                      name = name, track = ti }
                end
            end
        end
    end
    return out
end

local function timeline_fps(timeline)
    local ok, v = pcall(function()
        return timeline:GetSetting("timelineFrameRate")
    end)
    if ok and v then
        local n = tonumber(v)
        if n and n > 0 then return n end
    end
    return 0
end

-- "01:00:05:12" 或带丢帧的 "01:00:05;12" → 绝对帧数
local function tc_to_frames(tc, fps)
    if not tc or not fps or fps <= 0 then return nil end
    local hh, mm, ss, ff = tostring(tc):match("(%d+):(%d+):(%d+)[:;](%d+)")
    if not hh then return nil end
    return ((tonumber(hh) * 3600 + tonumber(mm) * 60 + tonumber(ss)) * fps)
           + tonumber(ff)
end

local function source_range(item, fps)
    if not fps or fps <= 0 then return 0, 0 end
    local okL, left  = pcall(function() return item:GetLeftOffset()  end)
    local okR, right = pcall(function() return item:GetRightOffset() end)
    if not (okL and okR) or not left or not right then return 0, 0 end
    if right <= left then return 0, 0 end
    return math.max(0, left / fps), right / fps
end

-- ══════════════════════════════════════════════════════════════════
-- 主程序
-- ══════════════════════════════════════════════════════════════════
local App = { S = {}, clips = {}, tlName = "" }

local function P(msg)
    print("[NamaBeauty] " .. tostring(msg))
end

function App:Log(msg)
    if self.S.log then
        pcall(function() self.S.log:AppendText(tostring(msg) .. "\n") end)
    end
    P(msg)
end

function App:Python()
    if self._py == nil then
        local cands = {
            "D:\\DaVinci Resolve\\fuscript.exe",
            "C:\\Program Files\\Blackmagic Design\\DaVinci Resolve\\fuscript.exe",
            "C:\\Program Files\\Blackmagic Design\\DaVinci Resolve Studio\\fuscript.exe",
        }
        self._py = ""
        for _, p in ipairs(cands) do
            if exists(p) then self._py = p; break end
        end
    end
    return self._py
end

function App:Main()
    P(string.rep("=", 62))
    P("NamaBeauty —— 皮肤 / 眼部美颜")
    P(string.rep("=", 62))

    local r = resolve
    if not r and type(Resolve) == "function" then r = Resolve() end
    if not r then
        P("错误：取不到 Resolve 对象。请从 工作区 → 脚本 菜单运行。")
        return
    end

    local pm = r:GetProjectManager()
    if not pm then P("错误：取不到项目管理器。"); return end

    local project = pm:GetCurrentProject()
    if not project then P("错误：请先打开一个项目。"); return end

    local timeline = project:GetCurrentTimeline()
    if not timeline then P("错误：请先打开一条时间线。"); return end

    self.project  = project
    self.timeline = timeline
    self.tlName   = timeline:GetName() or "未命名"

    local clips = collect_clips(timeline)
    if #clips == 0 then
        P("提示：时间线上没有带磁盘源文件的视频片段。")
        P("      生成器、复合片段这类没有源文件的素材无法处理。")
        return
    end
    self.clips = clips
    P("时间线：" .. self.tlName .. "，共 " .. #clips .. " 条可用视频片段")
    for i, c in ipairs(clips) do
        P("  <" .. i .. "> V" .. c.track .. "  " .. c.name)
    end

    local fusion = r:Fusion()
    if not fusion then P("错误：resolve:Fusion() 返回空。"); return end

    local ui = fusion.UIManager
    if not ui then P("错误：fusion.UIManager 为空。"); return end

    if type(bmd) ~= "table" then
        P("错误：bmd 全局不可用，无法建窗口。"); return
    end

    local dispatcher = bmd.UIDispatcher(ui)
    if not dispatcher then P("错误：UIDispatcher 返回空。"); return end
    self.S.dispatcher = dispatcher

    -- jobs / logs 目录由 Python 侧创建，这里不 mkdir（会闪黑窗）
    self:BuildUI(ui, dispatcher)
end

function App:BuildUI(ui, dispatcher)
    -- ── 读 config.json 的默认值 ──
    local cfg_txt = read_file(CONFIG)
    local params = {}
    for _, e in ipairs(EFFECTS) do params[e.key] = e.def end
    if cfg_txt ~= "" then
        local blk = cfg_txt:match('"params"%s*:%s*{(.-)}')
        if blk then
            for k, v in blk:gmatch('"([%w_]+)"%s*:%s*(-?%d+%.?%d*)') do
                params[k] = tonumber(v)
            end
        end
    end

    local clip_items = { ("全部 %d 条视频片段（逐条处理）"):format(#self.clips) }
    for _, c in ipairs(self.clips) do
        clip_items[#clip_items + 1] = ("<%d> %s"):format(#clip_items, c.name)
    end

    -- ── 参数行 ──
    local rows = { ui:Label{ ID = "SecSkin", Text = "── 皮肤 / 五官 ──", Weight = 0 } }
    for _, e in ipairs(EFFECTS) do
        rows[#rows + 1] = ui:HGroup{ ID = "Row_" .. e.key, Weight = 0,
            ui:Label{ ID = "L_" .. e.key, Text = e.label, Weight = 0.22 },
            ui:Slider{ ID = "S_" .. e.key, Minimum = 0, Maximum = 100,
                       Value = params[e.key] or e.def, Weight = 0.62 },
            ui:Label{ ID = "V_" .. e.key, Text = tostring(params[e.key] or e.def),
                       Weight = 0.16 },
        }
    end

    local out_rows = {
        ui:Label{ ID = "SepOut", Text = "── 输出 ──", Weight = 0 },
        ui:HGroup{ ID = "RowBT", Weight = 0,
            ui:Label{ ID = "L_BT", Text = "磨皮算法", Weight = 0.22 },
            ui:ComboBox{ ID = "BlurType", Weight = 0.5 },
            ui:CheckBox{ ID = "SkinDetect", Text = "只磨皮肤区域", Weight = 0.28 },
        },
        ui:HGroup{ ID = "RowPre", Weight = 0,
            ui:Label{ ID = "L_Pre", Text = "输出编码", Weight = 0.22 },
            ui:ComboBox{ ID = "Preset", Weight = 0.5 },
            ui:Label{ ID = "L_Pad", Text = "", Weight = 0.28 },
        },
        ui:CheckBox{ ID = "UseRange",
                     Text = "只处理时间线上用到的那一段（不勾则整段源媒体）",
                     Weight = 0, Checked = true },

        ui:Label{ ID = "SepPrev", Text = "── 预览（源素材原帧 ｜ 处理后）──",
                  Weight = 0 },
        ui:HGroup{ ID = "RowPrev", Weight = 0,
            ui:Button{ ID = "Prev",     Text = "预览当前帧",   Weight = 0.5 },
            ui:Button{ ID = "Live",     Text = "实时预览：关", Weight = 0.55 },
            ui:Button{ ID = "OpenPrev", Text = "打开上次预览", Weight = 0.5 },
        },
        ui:Label{ ID = "PrevMsg", Text = "", Weight = 0 },
    }

    local win = dispatcher:AddWindow({
        ID = "NamaBeautyWin",
        WindowTitle = "NamaBeauty 美颜",
        Geometry = { 240, 70, 600, 760 },
        Spacing = 8,
        StyleSheet = [[
            QWidget { font-size: 13px; }
            QPushButton#Go:enabled {
                background-color: rgba(70, 110, 145, 210);
            }
        ]],
        ui:VGroup{ ID = "Root", Spacing = 8,
            ui:Label{ ID = "Title", Text = "NamaBeauty 美颜", Weight = 0 },
            ui:Label{ ID = "TlName", Text = "时间线：" .. tostring(self.tlName),
                      Weight = 0 },
            ui:HGroup{ ID = "RowTarget", Weight = 0,
                ui:Label{ ID = "L_Clip", Text = "处理目标", Weight = 0.22 },
                ui:ComboBox{ ID = "Clip", Weight = 0.78 },
            },
            ui:VGroup{ ID = "Params", Weight = 1, Spacing = 4, unpack_(rows) },
            ui:VGroup{ ID = "OutBox", Weight = 0, Spacing = 4, unpack_(out_rows) },
            ui:Label{ ID = "L_Log", Text = "日志", Weight = 0 },
            ui:TextEdit{ ID = "Log", Weight = 0.8, ReadOnly = true },
            ui:HGroup{ ID = "Btns", Weight = 0,
                ui:Button{ ID = "Go",   Text = "开始处理" },
                ui:Button{ ID = "Quit", Text = "关闭" },
            },
        },
    })

    local itm = win:GetItems()

    itm.Clip:AddItems(clip_items)
    itm.Clip.CurrentIndex = 0

    local bt_labels = {}
    for _, pair in ipairs(BLUR_TYPES) do bt_labels[#bt_labels + 1] = pair[1] end
    itm.BlurType:AddItems(bt_labels)
    local bt_cfg = jget_num(cfg_txt, "blur_type", 1)
    itm.BlurType.CurrentIndex = 0
    for i, pair in ipairs(BLUR_TYPES) do
        if pair[2] == bt_cfg then itm.BlurType.CurrentIndex = i - 1 end
    end

    local pre_labels = {}
    for _, pair in ipairs(PRESETS) do pre_labels[#pre_labels + 1] = pair[1] end
    itm.Preset:AddItems(pre_labels)
    local pre_cfg = jget_str(cfg_txt, "preset", "h264_nvenc")
    itm.Preset.CurrentIndex = 0
    for i, pair in ipairs(PRESETS) do
        if pair[2] == pre_cfg then itm.Preset.CurrentIndex = i - 1 end
    end

    itm.SkinDetect.Checked = jget_bool(cfg_txt, "skin_detect", false)
    -- 存到 S 上，StartJob 里还要再取一次（不能跨函数用局部变量）
    self.S.cfg_txt = cfg_txt

    self.S.win  = win
    self.S.itm  = itm
    self.S.log  = itm.Log
    self.S.busy = false
    self.S.live = false

    self:Log("准备就绪，共 " .. #self.clips .. " 条片段")
    self:Log("引擎目录：" .. TOOL_ROOT)

    -- 滑块数值联动
    for _, e in ipairs(EFFECTS) do
        local lbl = itm["V_" .. e.key]
        win.On["S_" .. e.key].ValueChanged = function(ev)
            if lbl then lbl.Text = tostring(math.floor(tonumber(ev.Value) or 0)) end
        end
    end

    win.On.Go.Clicked = function(ev)
        if self.S.busy or self.S.previewing then return end
        self:StartJob()
    end

    win.On.Prev.Clicked = function(ev)
        if self.S.busy or self.S.previewing then return end
        local ok, err = pcall(function() self:StartPreview() end)
        if not ok then
            self:Log("预览启动失败：" .. tostring(err))
            self:PreviewDone()
        end
    end

    win.On.OpenPrev.Clicked = function(ev)
        if exists(PREV_OUT) then
            os.execute('start "" ' .. q(PREV_OUT))
            self:Log("已打开上次预览：" .. PREV_OUT)
        else
            self:Log("还没有预览图，先点「预览当前帧」")
        end
    end

    win.On.Live.Clicked = function(ev)
        local ok, err = pcall(function() self:ToggleLive() end)
        if not ok then self:Log("实时预览切换失败：" .. tostring(err)) end
    end

    win.On.Quit.Clicked = function(ev)
        self:StopLive(true)          -- 顺手收掉常驻预览进程，别留孤儿
        self.S.cancel = true
        dispatcher:ExitLoop()
    end

    -- 窗口自身也是带 ID 的控件，关闭事件挂在窗口 ID 上，
    -- 这样点右上角 X 也能退出消息循环，不会把 Resolve 卡死。
    win.On.NamaBeautyWin.Close = function(ev)
        self:StopLive(true)
        self.S.cancel = true
        dispatcher:ExitLoop()
    end

    -- ══════════════════════════════════════════════════════════
    -- ★ 头号坑：定时器必须显式创建再 :Start()。
    --   只写 dispatcher.On.Timeout = ... 是不会有超时事件的，
    --   结果就是后台跑完了，界面却永远停在「处理中」。
    --   Interval 单位是毫秒。
    -- ══════════════════════════════════════════════════════════
    local TIMER_ID = "NamaBeautyPoll"
    local okT, timer = pcall(function()
        return ui:Timer({ ID = TIMER_ID, Interval = 500, SingleShot = false })
    end)
    if not okT then
        P("警告：无法创建轮询定时器，进度不会自动刷新")
        timer = nil
    end

    -- 实时预览要跟手，150 ms 一轮才有那种「拖着滑块画面就变」的感觉
    local okL, ltimer = pcall(function()
        return ui:Timer({ ID = LIVE_TIMER, Interval = 150, SingleShot = false })
    end)
    if not okL then
        P("警告：无法创建实时预览定时器，该功能不可用")
        ltimer = nil
    end

    dispatcher.On.Timeout = function(ev)
        local who = ev and ev.who
        if who == LIVE_TIMER then
            local ok, err = pcall(function() self:TickLive() end)
            if not ok then P("!! TickLive 异常: " .. tostring(err)) end
            return
        end
        if who and who ~= TIMER_ID then return end
        local ok, err = pcall(function() self:Tick() end)
        if not ok then P("!! Tick 异常: " .. tostring(err)) end
    end

    win:Show()
    if timer  then pcall(function() timer:Start()  end) end
    if ltimer then pcall(function() ltimer:Start() end) end
    dispatcher:RunLoop()
    if timer  then pcall(function() timer:Stop()  end) end
    if ltimer then pcall(function() ltimer:Stop() end) end
    self:StopLive(true)          -- 循环退出后收尾，确保常驻进程不残留
    win:Hide()
end

-- ══════════════════════════════════════════════════════════════════
-- 启动处理任务：写 job → 后台起 worker → 交给 Tick 轮询
-- ══════════════════════════════════════════════════════════════════
function App:StartJob()
    local itm = self.S.itm
    local idx = itm.Clip.CurrentIndex or 0        -- 0 = 全部

    local targets = {}
    if idx <= 0 then
        for _, c in ipairs(self.clips) do targets[#targets + 1] = c end
    else
        targets[#targets + 1] = self.clips[idx]
    end

    if #targets == 0 then
        self:Log("错误：没有可处理的片段")
        return
    end

    local fps = timeline_fps(self.timeline)
    local p = {}
    for _, e in ipairs(EFFECTS) do
        local s = itm["S_" .. e.key]
        p[e.key] = s and math.floor(tonumber(s.Value) or 0) or 0
    end

    local btv   = BLUR_TYPES[(itm.BlurType.CurrentIndex or 0) + 1]
    local pv    = PRESETS[(itm.Preset.CurrentIndex or 0) + 1]
    local sd    = itm.SkinDetect.Checked
    local range = itm.UseRange and itm.UseRange.Checked or true
    local maxw  = jget_num(self.S.cfg_txt or "", "max_width", 0)

    self.S.queue = targets
    self.S.pos   = 0
    self.S.busy  = true
    self.S.params = p
    self.S.blur_type = btv and btv[2] or 1
    self.S.preset = pv and pv[2] or "h264_nvenc"
    self.S.skin  = sd
    self.S.range = range
    self.S.maxw  = maxw
    itm.Go.Enabled = false
    itm.Go.Text = "处理中…"
    self:NextTask()
end

function App:NextTask()
    local S = self.S
    S.pos = S.pos + 1
    if S.pos > #S.queue then
        S.busy = false
        S.itm.Go.Enabled = true
        S.itm.Go.Text = "开始处理"
        self:Log("全部完成")
        return
    end

    local c     = S.queue[S.pos]
    local stem  = c.name:gsub("%.[^%.]+$", "")
    local dir   = c.path:match("^(.*)\\[^\\]+$") or "."
    -- 输出扩展名必须跟预设走：ProRes 装不进 mp4 容器，写 .mp4 必失败。
    -- （命令行版 NamaBeauty_美颜_cli.py 早就这么做，这里补上。）
    local out_ext = (S.preset == "prores422") and ".mov" or ".mp4"
    local out     = dir .. "\\" .. stem .. "_beauty" .. out_ext

    local s, e = 0, 0
    if S.range then s, e = source_range(c.item, timeline_fps(self.timeline)) end

    -- 清掉上一次的结果文件，避免误判「已完成」
    local prog_file  = LOGS .. "\\progress.txt"
    local res_file   = LOGS .. "\\result.json"
    local job_file   = JOBS .. "\\job_lua.json"
    os.remove(res_file)
    os.remove(prog_file)
    S.last_progress = nil   -- 换任务时进度去重状态要重置

    local job = {
        src = c.path, out = out, start = s, ["end"] = e,
        params = S.params, blur_type = S.blur_type,
        skin_detect = S.skin, preset = S.preset,
        max_width = S.maxw, progress = prog_file, result = res_file,
    }
    write_file(job_file, jval(job))

    self:Log(("处理 <%d/%d> %s"):format(S.pos, #S.queue, c.name))
    self:Log("  输出 " .. out)

    local py = self:Python()
    if py == "" then
        self:Log("  错误：找不到 fuscript.exe / python.exe")
        S.busy = false
        S.itm.Go.Enabled = true
        S.itm.Go.Text = "开始处理"
        return
    end

    -- start "" /B：异步起子进程，不阻塞 Resolve 的消息泵
    local cmd = 'start "" /B ' .. q(py) .. ' -l python3 '
                .. q(WORKER) .. ' --job ' .. q(job_file)
    os.execute(cmd)
    S.running = true
    S.tick0 = os.time()
end

-- ══════════════════════════════════════════════════════════════════
-- 预览当前帧：只处理一帧，出「原图 ｜ 效果图」左右对比大图
-- ══════════════════════════════════════════════════════════════════
-- ══════════════════════════════════════════════════════════════════
-- 播放头落在哪条片段、对应源素材的第几秒
-- 单帧预览与实时预览都要用，抽出来免得两处逻辑跑偏
-- ══════════════════════════════════════════════════════════════════
function App:SourcePos()
    local fps = timeline_fps(self.timeline)

    -- 目标片段：优先取播放头压着的那一条
    local target, vitem = nil, nil
    local okV, vi = pcall(function() return self.timeline:GetCurrentVideoItem() end)
    if okV and vi then
        local okM, mpi = pcall(function() return vi:GetMediaPoolItem() end)
        if okM and mpi then
            local props = mpi:GetClipProperty()
            local path = (props and props["File Path"]) or ""
            for _, c in ipairs(self.clips) do
                if c.path == path then target = c; vitem = vi; break end
            end
        end
    end
    if not target then
        local idx = (self.S.itm and (self.S.itm.Clip.CurrentIndex or 0)) or 0
        target = (idx > 0 and self.clips[idx]) or self.clips[1]
        vitem = nil
    end
    if not target then return nil, nil end

    -- 换算成「源素材内的第几秒」
    local t = nil
    if vitem and fps > 0 then
        local okT, tc  = pcall(function() return self.timeline:GetCurrentTimecode() end)
        local okS, tc0 = pcall(function() return self.timeline:GetStartTimecode() end)
        local pf, pf0 = tc_to_frames(tc, fps), tc_to_frames(tc0, fps)
        if okT and okS and pf and pf0 then
            local rel = pf - pf0
            local okR, s0 = pcall(function() return vitem:GetStart() end)
            local okE, e0 = pcall(function() return vitem:GetEnd()   end)
            if okR and okE and s0 and e0 and e0 > s0
               and rel >= s0 and rel <= e0 then
                local okL, left = pcall(function() return vitem:GetLeftOffset() end)
                t = ((okL and left or 0) + (rel - s0)) / fps
            end
        end
    end
    if not t or t < 0 then
        local s, e = source_range(target.item, fps)
        t = (s + e) / 2          -- 播放头不在任何片段上时，退回片段中点
    end
    return target, t
end

function App:StartPreview()
    local itm = self.S.itm
    local target, t = self:SourcePos()
    if not target then
        self:Log("错误：时间线上没有可处理的片段")
        return
    end

    local p = {}
    for _, e in ipairs(EFFECTS) do
        local s = itm["S_" .. e.key]
        p[e.key] = s and math.floor(tonumber(s.Value) or 0) or 0
    end
    local btv = BLUR_TYPES[(itm.BlurType.CurrentIndex or 0) + 1]

    os.remove(PREV_RES)
    os.remove(PREV_PROG)

    local job = {
        src = target.path,
        preview = PREV_OUT,
        start = t,
        params = p,
        blur_type = btv and btv[2] or 1,
        skin_detect = itm.SkinDetect.Checked,
        max_faces = 4,
        preview_width = 1920,
        progress = PREV_PROG,
        result = PREV_RES,
    }
    write_file(PREV_JOB, jval(job))

    local py = self:Python()
    if py == "" then
        self:Log("错误：找不到 fuscript.exe")
        return
    end

    self:Log(("预览 <%s> 第 %.2f 秒处的帧…"):format(target.name, t))
    itm.Prev.Enabled = false
    itm.Prev.Text = "预览中…"
    itm.PrevMsg.Text = "大约 3 秒"
    self.S.previewing = true
    self.S.prev_t0 = os.time()

    os.execute('start "" /B ' .. q(py) .. ' -l python3 '
               .. q(WORKER) .. ' --job ' .. q(PREV_JOB))
end

function App:PreviewDone()
    self.S.previewing = false
    if self.S.itm then
        self.S.itm.Prev.Enabled = true
        self.S.itm.Prev.Text = "预览当前帧"
    end
end

-- ══════════════════════════════════════════════════════════════════
-- 实时预览
--
-- 单帧预览一次要 3 秒，拆开看是：
--     fuSetup + 人脸模型 + 美颜道具  1842 ms   ← 全是重复的初始化
--     抽一帧（ffmpeg 冷启动）          250 ms
--     渲染                              5 ms
--     编码成 JPEG（再起一个 ffmpeg）    154 ms
-- 真正干活只占 5 ms，其余都是在替「进程每次都重来一遍」买单。
--
-- 所以实时预览换个做法：nama_live.py 常驻，SDK 只初始化一次，
-- 之后每帧只花 渲染 ~10ms + 贴图 ~26ms，实测稳定 25~30 fps。
-- 本脚本这边只剩下「把参数推过去」这一件轻活。
-- ══════════════════════════════════════════════════════════════════
function App:WorkerAlive()
    local f = io.open(LIVE_PID, "r")
    if not f then return false end
    local pid = (f:read("*a") or ""):gsub("%s", "")
    f:close()
    if pid == "" then return false end
    local pipe = io.popen('tasklist /FI "PID eq ' .. pid .. '" /NH')
    if not pipe then return false end
    local out = pipe:read("*a") or ""
    pipe:close()
    return out:find(pid) ~= nil
end

function App:StartLive()
    if self.S.live then return end
    local target = select(1, self:SourcePos())
    if not target then
        self:Log("错误：时间线上没有可处理的片段")
        return
    end

    local py = self:Python()
    if py == "" then
        self:Log("错误：找不到 fuscript.exe")
        return
    end

    os.remove(LIVE_QUIT)
    os.remove(LIVE_JSON)
    write_file(LIVE_HB, tostring(os.time()))

    if self:WorkerAlive() then
        self:Log("复用已在运行的实时预览进程")
    else
        self:Log("启动实时预览进程…（首次加载美颜引擎约 2 秒）")
        os.execute('start "" /B ' .. q(py) .. ' -l python3 ' .. q(LIVE_SCRIPT))
    end

    self.S.live     = true
    self.S.live_key = nil
    if self.S.itm and self.S.itm.Live then
        self.S.itm.Live.Text = "实时预览：开"
    end
    self:Log("实时预览已开启：拖动滑块即刻生效，对比窗口在另一个窗口里")
    self:Log("  想看别的位置，把播放头挪过去即可（换帧约 0.25 秒）")
end

function App:StopLive(silent)
    if not self.S.live then return end
    self.S.live     = false
    self.S.live_key = nil
    write_file(LIVE_QUIT, "1")       -- 常驻进程看到就自己收工
    os.remove(LIVE_HB)
    if self.S.itm and self.S.itm.Live then
        self.S.itm.Live.Text = "实时预览：关"
    end
    if not silent then self:Log("实时预览已关闭") end
end

function App:ToggleLive()
    if self.S.live then self:StopLive() else self:StartLive() end
end

-- 实时预览的轮询体：只做两件轻活——写心跳、把变化推给常驻进程。
-- 参数没变就什么都不发，所以停在原地时开销接近零。
function App:TickLive()
    if not self.S.live then return end

    -- 心跳无条件写：常驻进程靠它判断面板是否还开着
    write_file(LIVE_HB, tostring(os.time()))

    local itm = self.S.itm
    if not itm then return end

    local target, t = self:SourcePos()
    if not target then return end
    t = math.floor((t or 0) * 100 + 0.5) / 100   -- 归一到 0.01 秒，免得浮点抖动触发重渲染

    local p, sig = {}, {}
    for _, e in ipairs(EFFECTS) do
        local s = itm["S_" .. e.key]
        local v = (s and math.floor(tonumber(s.Value) or 0)) or 0
        p[e.key] = v
        sig[#sig + 1] = tostring(v)
    end
    local btv = BLUR_TYPES[(itm.BlurType.CurrentIndex or 0) + 1]
    local bt  = (btv and btv[2]) or 1
    local sd  = itm.SkinDetect.Checked and 1 or 0

    -- 签名用固定顺序自己拼：jval 走的是 pairs，键序不保证稳定，
    -- 直接拿编码结果做比较会「内容没变但字符串不同」，白白触发重渲染。
    local key = target.path .. "|" .. string.format("%.2f", t)
                .. "|" .. bt .. "|" .. sd .. "|" .. table.concat(sig, ",")
    if key == self.S.live_key then return end
    self.S.live_key = key

    write_file(LIVE_JSON, jval({
        src = target.path, pos = t, params = p,
        blur_type = bt, skin_detect = (sd == 1),
    }))
end

-- ══════════════════════════════════════════════════════════════════
-- 定时器回调：轮询进度 / 结果，刷新界面
-- ══════════════════════════════════════════════════════════════════
function App:Tick()
    if self.S.previewing then
        local ok, err = pcall(function() self:TickPreview() end)
        if not ok then
            P("!! TickPreview 异常: " .. tostring(err))
            self:PreviewDone()
        end
        return
    end
    if not self.S.running then return end

    -- 读进度文件（worker 每次整体覆写一行）
    local pf = io.open(LOGS .. "\\progress.txt", "r")
    if pf then
        local line = pf:read("*l")
        pf:close()
        -- 同一行只追加一次：worker 覆写的是同一行内容，以前每 500ms 无条件
        -- AppendText，10 分钟的任务能在面板里堆出上千行重复进度。
        if line and line ~= "" and line ~= self.S.last_progress then
            self.S.last_progress = line
            self.S.itm.Log:AppendText("  " .. line .. "\n")
        end
    end

    -- 结果文件出现 = worker 已结束
    local rf = io.open(LOGS .. "\\result.json", "r")
    if not rf then
        -- 超时保护：20 分钟还没结果就放弃，免得界面永远转圈
        if os.time() - (self.S.tick0 or os.time()) > 1200 then
            self:Log("  超时：20 分钟没有结果，已放弃")
            self.S.running = false
            self.S.busy = false
            self.S.itm.Go.Enabled = true
            self.S.itm.Go.Text = "开始处理"
        end
        return
    end
    local txt = rf:read("*a")
    rf:close()

    self.S.running = false

    local ok = txt:find('"ok"%s*:%s*true') ~= nil
    local outp = txt:match('"out"%s*:%s*"([^"]*)"')
    local frames = txt:match('"frames"%s*:%s*(%d+)')
    local faces = txt:match('"faces"%s*:%s*(%d+)')
    local err = txt:match('"error"%s*:%s*"([^"]*)"')
    -- JSON 里的反斜杠是转义的，还原回真正的 Windows 路径才能用
    if outp then outp = outp:gsub("\\\\", "\\") end

    if ok then
        self:Log(("  完成 %s 帧，检出人脸 %s"):format(frames or "?", faces or "?"))
        -- 导回媒体池
        local done = false
        pcall(function()
            local ms = resolve:GetMediaStorage()
            if ms and outp and outp ~= "" then
                local added = ms:AddItemListToMediaPool(outp)
                done = added ~= nil
            end
        end)
        if done then
            self:Log("  已导入媒体池")
        else
            self:Log("  未自动导入，请手动把成品拖进媒体池")
        end
    else
        self:Log("  失败：" .. (err or "未知错误"))
    end

    -- 处理下一条
    self:NextTask()
end

-- ══════════════════════════════════════════════════════════════════
-- 预览轮询：结果文件一出现就打开对比大图
-- ══════════════════════════════════════════════════════════════════
function App:TickPreview()
    local rf = io.open(PREV_RES, "r")
    if not rf then
        if os.time() - (self.S.prev_t0 or os.time()) > 180 then
            self:Log("  预览超时：3 分钟没有结果，已放弃")
            self:PreviewDone()
        end
        return
    end
    local txt = rf:read("*a")
    rf:close()

    local ok   = txt:find('"ok"%s*:%s*true') ~= nil
    local outp = txt:match('"preview"%s*:%s*"([^"]*)"')
    local err  = txt:match('"error"%s*:%s*"([^"]*)"')
    if outp then outp = outp:gsub("\\\\", "\\") end

    if ok and outp and outp ~= "" then
        self:Log("  预览已生成，已用系统看图器打开：" .. outp)
        self.S.itm.PrevMsg.Text = "已生成"
        os.execute('start "" ' .. q(outp))
    else
        self:Log("  预览失败：" .. (err or "未知错误"))
        self.S.itm.PrevMsg.Text = "失败"
    end
    self:PreviewDone()
end

App:Main()
