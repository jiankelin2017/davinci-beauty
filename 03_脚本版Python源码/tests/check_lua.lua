local out = io.open("C:/ProgramData/NamaBeauty/logs/lua_check.txt", "w")
local f, err = loadfile("C:/ProgramData/NamaBeauty/tests/_syntax_check.lua")
if f then out:write("[OK] Lua 语法通过\n") else out:write("[ERR] " .. tostring(err) .. "\n") end
out:close()
