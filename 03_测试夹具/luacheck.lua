-- 只编译不执行，纯语法检查
local path = [[C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Scripts\Utility\NamaBeauty_美颜.lua]]
local f, err = loadfile(path)
if f then
    print("SYNTAX_OK: 语法检查通过")
else
    print("SYNTAX_FAIL: " .. tostring(err))
end
