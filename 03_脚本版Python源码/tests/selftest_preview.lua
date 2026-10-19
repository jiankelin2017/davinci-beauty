-- 定向自检：只验证新增逻辑，不建窗口，跑完即退
local function L(s) print("[T] " .. tostring(s)) end

-- ---------- 1. 时间码换算 ----------
local function tc_to_frames(tc, fps)
    if not tc or not fps or fps <= 0 then return nil end
    local hh, mm, ss, ff = tostring(tc):match("(%d+):(%d+):(%d+)[:;](%d+)")
    if not hh then return nil end
    return ((tonumber(hh) * 3600 + tonumber(mm) * 60 + tonumber(ss)) * fps)
           + tonumber(ff)
end

L("01:00:05:12 @25  -> " .. tostring(tc_to_frames("01:00:05:12", 25)) .. "  (期望 150137)")
L("01:00:00;00 @30  -> " .. tostring(tc_to_frames("01:00:00;00", 30)) .. "  (期望 108000)")
L("nil               -> " .. tostring(tc_to_frames(nil, 25)))
L("坏字符串          -> " .. tostring(tc_to_frames("abc", 25)))

-- ---------- 2. JSON 里的路径反转义 ----------
local raw = [[{"ok": true, "preview": "C:\\ProgramData\\NamaBeauty\\logs\\x.jpg"}]]
local p = raw:match('"preview"%s*:%s*"([^"]*)"')
L("JSON 原样取出 -> " .. tostring(p))
if p then L("反转义后      -> " .. p:gsub("\\\\", "\\")) end

-- ---------- 3. Resolve API 可用性 ----------
local r = resolve
if not r and type(Resolve) == "function" then r = Resolve() end
local proj = r:GetProjectManager():GetCurrentProject()
local tl = proj:GetCurrentTimeline()
L("时间线 = " .. tostring(tl:GetName()))

local fps = tonumber(tl:GetSetting("timelineFrameRate") or 0)
L("timelineFrameRate = " .. tostring(fps))

local ok1, tc  = pcall(function() return tl:GetCurrentTimecode() end)
L("GetCurrentTimecode  ok=" .. tostring(ok1) .. " => " .. tostring(tc))
local ok2, tc0 = pcall(function() return tl:GetStartTimecode() end)
L("GetStartTimecode    ok=" .. tostring(ok2) .. " => " .. tostring(tc0))

local ok3, vi = pcall(function() return tl:GetCurrentVideoItem() end)
L("GetCurrentVideoItem ok=" .. tostring(ok3) .. " => " .. tostring(vi))

if vi then
    local _, s0 = pcall(function() return vi:GetStart() end)
    local _, e0 = pcall(function() return vi:GetEnd() end)
    local _, lo = pcall(function() return vi:GetLeftOffset() end)
    L("  GetStart=" .. tostring(s0) .. "  GetEnd=" .. tostring(e0)
      .. "  GetLeftOffset=" .. tostring(lo))

    local pf, pf0 = tc_to_frames(tc, fps), tc_to_frames(tc0, fps)
    if pf and pf0 then
        local rel = pf - pf0
        L("  播放头相对时间线 = " .. rel .. " 帧")
        L("  片段区间 = [" .. tostring(s0) .. ", " .. tostring(e0) .. "]")
        if e0 and s0 and e0 > s0 and rel >= s0 and rel <= e0 then
            local t = ((lo or 0) + (rel - s0)) / fps
            L("  ★ 换算出的源素材时间点 = " .. string.format("%.3f", t) .. " 秒")
        else
            L("  ! 播放头不在该片段区间内，会退回片段中点")
        end
    end

    local mpi = vi:GetMediaPoolItem()
    if mpi then
        local pp = mpi:GetClipProperty()
        L("  片段文件 = " .. tostring(pp and pp["File Path"]))
    end
end

L("自检结束")
