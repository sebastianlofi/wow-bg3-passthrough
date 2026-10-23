-- The wowbg3 bg3le native plugin (bg3/plugin) seen from Lua. A plugin cannot add Lua
-- functions, so it answers one reserved physics query instead: RaycastClosest with
-- type 0, include 0 and context 0x57423301, the opcode in the exclude mask and six
-- float arguments. The call runs synchronously on this context's own thread. Results
-- sit in the plugin's per-side Out block (address in its settings), read with Peek:
--   +0 Err (int32), +4 R[4] (int32), +20 F[16] (float), +84 Calls (uint32), +88 Why (text)
local N = {}
local NAME, CHANNEL = "wowbg3", 0x57423301
N.Op = {Ping = 1, Stamp = 2, Restore = 3, Rays = 4, RaysOff = 5, RaysStats = 6, CamInstall = 7,
    CamPose = 8, CamOff = 9, CamStats = 10, CountOn = 11, CountRead = 12, CountOff = 13, Basis = 14,
    Follow = 15, Status = 16, Limits = 17, CamHold = 18, GridSurvey = 19, Columns = 20, SceneCounts = 21,
    Lead = 22, Pool = 23, Place = 24, PoolRestore = 25, ProxyCheck = 26, Bounds = 27, PickRegion = 28,
    LeadSource = 29, SpatialGrid = 30, SightSync = 31, PoolSurfaces = 60, PoolFree = 61}

local function get(id)
    local ok, v = pcall(Ext.Plugins.Get, NAME, id)
    return ok and v or nil
end

-- {Loaded, Version, Channel (1 server, 2 client), Frames}, or nil and why.
function N.Info()
    local ok, list = pcall(Ext.Plugins.List)
    if not ok or type(list) ~= "table" then return nil, "Ext.Plugins unavailable" end
    for _, p in ipairs(list) do
        if p.Name == NAME then
            if not p.Loaded then return nil, "plugin failed: " .. tostring(p.Error) end
            return {Loaded = true, Version = p.Version, Channel = get("channel"), Frames = get("frames"),
                FollowFrames = get("follow_frames")}
        end
    end
    return nil, "wowbg3 plugin not installed"
end

local function u32(v) return v & 0xffffffff end
local function outAddress(client)
    local lo, hi = get(client and "out_client_lo" or "out_server_lo"), get(client and "out_client_hi" or "out_server_hi")
    assert(math.type(lo) == "integer" and math.type(hi) == "integer", "wowbg3 plugin settings missing")
    return u32(lo) | (u32(hi) << 32)
end
local function peek32(at) return u32(assert(Ext._Internal.Peek(at, 4), "unreadable plugin output")) end
local function i32(v) return v >= 0x80000000 and v - 0x100000000 or v end
local function f32(v) return (string.unpack("<f", string.pack("<I4", v))) end

-- One plugin command. Returns out {Err, R = {4}, F = {16}, Why}; errors if the plugin
-- did not take the call. Callers check out.Err (0 = done).
function N.Call(op, a1, a2, a3, b1, b2, b3)
    local client = Ext.IsClient()
    local info, why = N.Info()
    assert(info, why)
    local bit = client and 2 or 1
    assert(math.type(info.Channel) == "integer" and info.Channel & bit ~= 0,
        "wowbg3 channel not hooked on the " .. (client and "client" or "server"))
    local at = outAddress(client)
    local before = peek32(at + 84)
    local args = {a1 or 0, a2 or 0, a3 or 0, b1 or 0, b2 or 0, b3 or 0}
    for i = 1, 6 do assert(type(args[i]) == "number" and args[i] == args[i], "plugin argument " .. i .. " is not a number") end
    Ext._Internal.PhysicsQuery(0, client, args[1], args[2], args[3], args[4], args[5], args[6], 0, 0, 0, 0, 0,
        0, 0, op, CHANNEL)
    assert(peek32(at + 84) == u32(before + 1), "wowbg3 plugin did not take the call (channel lost?)")
    local out = {Err = i32(peek32(at)), R = {}, F = {}}
    for i = 1, 4 do out.R[i] = i32(peek32(at + 4 * i)) end
    for i = 1, 16 do out.F[i] = f32(peek32(at + 16 + 4 * i)) end
    out.Why = out.Err ~= 0 and Ext._Internal.PeekString(at + 88) or nil
    return out
end

-- Call that must succeed: returns out, or raises the plugin's reason.
function N.Must(op, ...)
    local out = N.Call(op, ...)
    if out.Err ~= 0 then error("wowbg3: " .. tostring(out.Why or out.Err), 2) end
    return out
end

-- A 47-bit address as three exact 16-bit floats.
function N.Pieces(address)
    assert(math.type(address) == "integer" and address > 0 and address < (1 << 48), "Bad address")
    return address & 0xffff, (address >> 16) & 0xffff, (address >> 32) & 0xffff
end

-- Writes 16 numbers (text) for CamPose.
function N.SaveView(view)
    assert(type(view) == "table" and #view == 16, "View must be 16 numbers")
    local parts = {}
    for i = 1, 16 do parts[i] = string.format("%.9g", view[i]) end
    assert(Ext.IO.SaveFile("wow-bg3/native-camera-view.txt", table.concat(parts, " ")), "Cannot write the view file")
end

return N
