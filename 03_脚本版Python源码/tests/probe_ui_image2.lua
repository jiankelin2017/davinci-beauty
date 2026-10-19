-- 探针 v2：让窗口停留 8 秒，肉眼确认哪几种写法能真的显示图片
local PNG = "C:\\Users\\Administrator\\.workbuddy\\clipboard-images\\clipboard-2026-10-14T12-38-58-817Z-9d2f2efc.png"

local function L(s) print("[probe] " .. tostring(s)) end

local r = resolve
if not r and type(Resolve) == "function" then r = Resolve() end
local ui = r:Fusion().UIManager
local d  = bmd.UIDispatcher(ui)

local ic = ui:Icon({ ID = "ic", File = PNG })

-- 第三种候选写法
local imgObj = nil
local okI, resI = pcall(function() return ui:Image(PNG) end)
L("ui:Image(path) -> ok=" .. tostring(okI) .. "  " .. tostring(resI))
if okI then imgObj = resI end

local w = d:AddWindow({
    ID = "ProbeWin2",
    WindowTitle = "NamaBeauty 内嵌图片探针 —— 8 秒后自动关闭",
    Geometry = { 220, 120, 880, 640 },
    ui:VGroup{
        ui:Label{ ID = "H", Text = "请看下面三处，告诉我哪些出现了截图画面：", Weight = 0 },
        ui:Label{ ID = "A", Text = "【A】Label + Icon：", Weight = 0 },
        ui:Label{ ID = "AIMG", Text = "", Weight = 1, Icon = ic },
        ui:Label{ ID = "B", Text = "【B】Button + Icon：", Weight = 0 },
        ui:Button{ ID = "BIMG", Text = "", Icon = ic, Weight = 0 },
        ui:Label{ ID = "C", Text = "【C】Label + Image：", Weight = 0 },
        ui:Label{ ID = "CIMG", Text = "", Weight = 0, Image = imgObj },
        ui:Label{ ID = "F", Text = "— 8 秒后自动关闭 —", Weight = 0 },
    },
})

local timer = ui:Timer({ ID = "PT2", Interval = 8000, SingleShot = true })
d.On.Timeout = function(ev) d:ExitLoop() end
timer:Start()
w:Show()
d:RunLoop()
w:Hide()
L("探针结束")
