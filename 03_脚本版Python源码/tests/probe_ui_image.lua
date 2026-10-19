-- 探针：达芬奇原生 UI 能否在窗口里内嵌显示一张图片
local PNG = "C:\\Users\\Administrator\\.workbuddy\\clipboard-images\\clipboard-2026-10-14T12-38-58-817Z-9d2f2efc.png"

local function L(s) print("[probe] " .. tostring(s)) end

local r = resolve
if not r and type(Resolve) == "function" then r = Resolve() end
local fusion = r:Fusion()
local ui = fusion.UIManager
local d  = bmd.UIDispatcher(ui)

L("ui.Icon      = " .. type(ui.Icon))
L("ui.Pixmap    = " .. type(ui.Pixmap))
L("ui.Image     = " .. type(ui.Image))
L("ui.Label     = " .. type(ui.Label))
L("ui.Button    = " .. type(ui.Button))

local ic = nil
local ok1, res1 = pcall(function() return ui:Icon({ ID = "ic", File = PNG }) end)
L("Icon{ID,File} -> ok=" .. tostring(ok1) .. "  " .. tostring(res1))
if ok1 then ic = res1 end

local ok2, res2 = pcall(function() return ui:Icon({ ID = "ic2", Filename = PNG }) end)
L("Icon{ID,Filename} -> ok=" .. tostring(ok2) .. "  " .. tostring(res2))

local ok3, res3 = pcall(function() return ui:Icon({ "ic3", PNG }) end)
L("Icon{id,path} -> ok=" .. tostring(ok3) .. "  " .. tostring(res3))

local wok, werr = pcall(function()
    local w = d:AddWindow({
        ID = "ProbeWin",
        WindowTitle = "NamaBeauty UI 探针（2 秒后自动关闭）",
        Geometry = { 260, 200, 460, 300 },
        ui:VGroup{
            ui:Label{ ID = "T", Text = "尝试内嵌图片（下方应为截图）", Weight = 0 },
            ui:Label{ ID = "IMG", Text = "(若这里是空白说明不支持)", Weight = 1, Icon = ic },
            ui:Label{ ID = "END", Text = "— 结束 —", Weight = 0 },
        },
    })
    local itm = w:GetItems()
    L("Label.IMG 创建后 Icon 属性 = " .. tostring(itm.IMG and itm.IMG.Icon))
    L("Label.IMG 全部键 = " .. tostring(itm.IMG))

    local timer = ui:Timer({ ID = "PT", Interval = 2000, SingleShot = true })
    d.On.Timeout = function(ev) d:ExitLoop() end
    timer:Start()
    w:Show()
    d:RunLoop()
    w:Hide()
end)
L("窗口构建 -> ok=" .. tostring(wok) .. "  " .. tostring(werr))
