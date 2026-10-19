-- 探针 v3：窗口停留 30 秒，期间由外部截屏取证
local PNG = "C:\\Users\\Administrator\\.workbuddy\\clipboard-images\\clipboard-2026-10-14T12-38-58-817Z-9d2f2efc.png"

local function L(s) print("[probe] " .. tostring(s)) end

local r = resolve
if not r and type(Resolve) == "function" then r = Resolve() end
local ui = r:Fusion().UIManager
local d  = bmd.UIDispatcher(ui)

local ic     = ui:Icon({ ID = "ic", File = PNG })
local imgObj = ui:Image(PNG)

local w = d:AddWindow({
    ID = "ProbeWin3",
    WindowTitle = "NAMAPROBE-3",
    Geometry = { 200, 100, 900, 660 },
    ui:VGroup{
        ui:Label{ ID = "H",  Text = "A = Label+Icon / B = Button+Icon / C = Label+Image", Weight = 0 },
        ui:Label{ ID = "A",  Text = "=== A below ===", Weight = 0 },
        ui:Label{ ID = "AIMG", Text = "", Weight = 1, Icon = ic },
        ui:Label{ ID = "B",  Text = "=== B below ===", Weight = 0 },
        ui:Button{ ID = "BIMG", Text = "", Icon = ic, Weight = 0 },
        ui:Label{ ID = "C",  Text = "=== C below ===", Weight = 0 },
        ui:Label{ ID = "CIMG", Text = "", Weight = 0, Image = imgObj },
        ui:Label{ ID = "F",  Text = "=== end ===", Weight = 0 },
    },
})

local timer = ui:Timer({ ID = "PT3", Interval = 30000, SingleShot = true })
d.On.Timeout = function(ev) d:ExitLoop() end
timer:Start()
w:Show()
L("窗口已显示")
d:RunLoop()
w:Hide()
L("窗口已关闭")
