-- 按前端的方式写一个预览 job（jval 从主脚本原样复制），交给 worker 跑
local ROOT = "C:\\ProgramData\\NamaBeauty"

-- ===== jval（与主脚本一致）=====
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

local job = {
    src = "D:\\c\\2026-10-12 19-02-57.mp4",
    preview = ROOT .. "\\logs\\preview_compare.jpg",
    start = 436.8,
    params = { blur_level = 70, clarity = 0, color_level = 15, red_level = 30,
               sharpen = 0, face_threed = 0, eye_bright = 0,
               remove_pouch_strength = 0 },
    blur_type = 1,
    skin_detect = false,
    max_faces = 4,
    preview_width = 1920,
    progress = ROOT .. "\\logs\\progress_preview.txt",
    result = ROOT .. "\\logs\\result_preview.json",
}

local txt = jval(job)
local f = io.open(ROOT .. "\\jobs\\job_preview.json", "w")
f:write(txt)
f:close()
print("[T] 已写入 job_preview.json：")
print(txt)
