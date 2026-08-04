--[[
psxref.lua -- drive the real TR1 PSX disc headlessly and capture reference frames.

WHY: this project's visual authority has been two fixed playthrough VIDEOS in
res/. They cannot be posed, so questions like "is her neck covered by hair from
behind?" get answered from a blurry frame someone else chose, or not at all.
This makes the original game a poseable reference: same room, same angle as our
Jaguar build, on demand.

See PSX_DYNAMIC_RE.md for the emulator's gotchas. The one that matters here:
`AfterPollingCleanup` is a ONE-SHOT hook -- it MUST re-arm itself or it runs
exactly once, which is indistinguishable from a hang.

USAGE
  pcsx-redux -no-ui -stdout -bios <bios> -iso <cue> \
      -dofile tools/psxref.lua -run
Configure with env vars read below (PSXREF_OUT, PSXREF_SHOTS, PSXREF_KEYS).

Screenshots are written as RAW + a .meta sidecar; tools/psxref_png.py converts.
--]]

local B = PCSX.CONSTS.PAD.BUTTON
local pad = PCSX.SIO0.slots[1].pads[1]

local OUT   = os.getenv('PSXREF_OUT')   or '/tmp/psxref'
-- "frame:name,frame:name" -- when to grab a screenshot
local SHOTS = os.getenv('PSXREF_SHOTS') or '600:boot'
-- "frame:BUTTON:holdframes,..." -- BUTTON is a PCSX.CONSTS.PAD.BUTTON name
local KEYS  = os.getenv('PSXREF_KEYS')  or ''

local function parseShots(s)
  local t = {}
  for f, n in string.gmatch(s, '(%d+):([%w_]+)') do t[tonumber(f)] = n end
  return t
end

local function parseKeys(s)
  local t = {}
  for f, b, h in string.gmatch(s, '(%d+):(%u[%u%d]*):(%d+)') do
    t[#t+1] = { at = tonumber(f), btn = B[b], hold = tonumber(h), name = b }
    if B[b] == nil then print('[psxref] UNKNOWN BUTTON: ' .. b) end
  end
  return t
end

local shots   = parseShots(SHOTS)
local keys    = parseKeys(KEYS)
local lastAt  = 0
for f, _ in pairs(shots) do if f > lastAt then lastAt = f end end
for _, k in ipairs(keys) do if k.at + k.hold > lastAt then lastAt = k.at + k.hold end end

local function grab(name, frame)
  local ss = PCSX.GPU.takeScreenShot()
  local raw = OUT .. '/' .. name .. '.raw'
  local f = Support.File.open(raw, 'TRUNCATE')
  f:writeMoveSlice(ss.data)
  f:close()
  local m = Support.File.open(OUT .. '/' .. name .. '.meta', 'TRUNCATE')
  m:write(string.format('%d %d %d %d\n', ss.width, ss.height, ss.bpp, frame))
  m:close()
  print(string.format('[psxref] shot %-14s frame %-6d %dx%d bpp=%d',
                      name, frame, ss.width, ss.height, ss.bpp))
end

-- released[] tracks pending releases so a hold spans N frames
local released = {}
local i = 0
local function tick()
  -- RE-ARM FIRST, AND pcall EVERYTHING ELSE. UI::tick() wraps this handler in
  -- its own pcall and swallows the error (`catch (...) {}`), so ANY throw below
  -- would skip a re-arm placed at the end and the loop would stop dead after
  -- one iteration -- identical in appearance to the emulator hanging. That is
  -- exactly what `takeScreenShot()` does without -softgpu: the base GPU throws
  -- "Not yet implemented", and only src/gpu/soft implements it.
  AfterPollingCleanup = tick
  i = i + 1

  -- HEARTBEAT TO A FILE, NOT print(). PCSX::TUI::addLuaLog() is an EMPTY
  -- FUNCTION (src/main/textui.cc), so every print() from inside the running
  -- emulator is silently discarded under -no-ui. Only output emitted at
  -- -dofile time reaches stdout. Without this file you cannot tell "the hook
  -- never ran" from "the hook ran and said nothing".
  if i % 30 == 0 then
    local h = io.open(OUT .. '/heartbeat.txt', 'w')
    if h then h:write(tostring(i) .. '\n'); h:close() end
  end

  for _, k in ipairs(keys) do
    if k.btn then
      if i == k.at then
        pad.setOverride(k.btn)            -- setOverride = FORCE PRESSED
        released[k.at + k.hold] = released[k.at + k.hold] or {}
        table.insert(released[k.at + k.hold], k.btn)
        print(string.format('[psxref] frame %-6d press   %s', i, k.name))
      end
    end
  end
  if released[i] then
    for _, b in ipairs(released[i]) do pad.clearOverride(b) end
    print(string.format('[psxref] frame %-6d release', i))
  end

  if shots[i] then
    local ok, err = pcall(grab, shots[i], i)
    if not ok then print('[psxref] SHOT FAILED at ' .. i .. ': ' .. tostring(err)) end
  end

  if i >= lastAt + 30 then
    print('[psxref] done at frame ' .. i)
    PCSX.quit(0)
  end
end

print(string.format('[psxref] out=%s  shots=%s  keys=%s  runs to ~%d',
                    OUT, SHOTS, KEYS, lastAt + 30))
AfterPollingCleanup = tick
