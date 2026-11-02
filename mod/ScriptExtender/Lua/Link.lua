-- WOW-BG3 link protocol v1 for Lua 5.3: strict codec and stop-and-wait endpoint.
-- Mirrors bridge/src/wire.rs. Pure Lua: no Ext, io or os calls, so it is tested outside BG3.
-- Fail closed: anything Rust rejects is rejected. A few non-canonical forms Rust tolerates
-- but never emits (array-form structs, map-form enums) are also rejected here.
local M = {}
M.VERSION = 1
M.MAX_FRAME = 65536
M.MAX_ACTORS = 64
M.MAX_PATH = 256
M.MAX_SEQUENCE = (1 << 53) - 1
M.null = setmetatable({}, {__tostring = function() return "null" end})
local U16, U32, U64 = 0xFFFF, 0xFFFFFFFF, math.maxinteger

local function fail(why) error({link = why}, 0) end
local function need(ok, why) if not ok then fail(why) end end
-- Converts internal failures to (nil, message); programming errors keep their text.
local function guard(fn, ...)
    local ok, a, b = pcall(fn, ...)
    if ok then return a, b end
    return nil, type(a) == "table" and a.link or tostring(a)
end
M.guard = guard

local function utf8_ok(s)
    local i, n = 1, #s
    while i <= n do
        local c = s:byte(i)
        if c < 0x80 then
            i = i + 1
        else
            local len, min
            if c >= 0xC2 and c <= 0xDF then len, min = 2, 0x80
            elseif c >= 0xE0 and c <= 0xEF then len, min = 3, 0x800
            elseif c >= 0xF0 and c <= 0xF4 then len, min = 4, 0x10000
            else return false end
            if i + len - 1 > n then return false end
            local cp = c & (0x7F >> len)
            for k = 1, len - 1 do
                local b = s:byte(i + k)
                if b < 0x80 or b > 0xBF then return false end
                cp = (cp << 6) | (b & 0x3F)
            end
            if cp < min or cp > 0x10FFFF or (cp >= 0xD800 and cp <= 0xDFFF) then return false end
            i = i + len
        end
    end
    return true
end
M.utf8_ok = utf8_ok

local function utf8_char(cp)
    if cp < 0x80 then return string.char(cp) end
    if cp < 0x800 then return string.char(0xC0 | (cp >> 6), 0x80 | (cp & 0x3F)) end
    if cp < 0x10000 then
        return string.char(0xE0 | (cp >> 12), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
    end
    return string.char(0xF0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3F), 0x80 | ((cp >> 6) & 0x3F), 0x80 | (cp & 0x3F))
end

-- FNV-1a over exact body bytes. Detects torn/mixed writes; it is not authentication.
local function checksum(s)
    local h = 2166136261
    for i = 1, #s do h = ((h ~ s:byte(i)) * 16777619) & U32 end
    return h
end
M.checksum = checksum

---------------------------------------------------------------------------- JSON parse
-- Strict RFC 8259 parser. Objects reject duplicate keys (serde does too). Numbers keep
-- whether they were written as integers, because serde rejects 1.0 for an integer field.
local MAX_DEPTH = 32
local SIMPLE = {['"'] = '"', ['\\'] = '\\', ['/'] = '/', b = '\b', f = '\f', n = '\n', r = '\r', t = '\t'}
local function parse(s)
    local pos, depth = 1, 0
    local value
    local function ws() local _, e = s:find("^[ \t\n\r]*", pos); pos = e + 1 end
    local function hex4(p)
        local h = s:sub(p, p + 3)
        need(#h == 4 and h:find("^[0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f][0-9A-Fa-f]$"), "invalid unicode escape")
        return tonumber(h, 16)
    end
    local function str()
        pos = pos + 1
        local parts = {}
        while true do
            local a, b = s:find('^[^"\\\0-\31]+', pos)
            if a then parts[#parts + 1] = s:sub(a, b); pos = b + 1 end
            local c = s:sub(pos, pos)
            if c == '"' then
                pos = pos + 1
                return table.concat(parts)
            elseif c == "\\" then
                local e = s:sub(pos + 1, pos + 1)
                if SIMPLE[e] then
                    parts[#parts + 1] = SIMPLE[e]; pos = pos + 2
                elseif e == "u" then
                    local cp = hex4(pos + 2); pos = pos + 6
                    if cp >= 0xD800 and cp <= 0xDBFF then
                        need(s:sub(pos, pos + 1) == "\\u", "lone leading surrogate")
                        local lo = hex4(pos + 2); pos = pos + 6
                        need(lo >= 0xDC00 and lo <= 0xDFFF, "invalid surrogate pair")
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00)
                    else
                        need(cp < 0xDC00 or cp > 0xDFFF, "lone trailing surrogate")
                    end
                    parts[#parts + 1] = utf8_char(cp)
                else
                    fail("invalid escape")
                end
            else
                fail("unterminated string or raw control character")
            end
        end
    end
    local function num()
        local start = pos
        if s:sub(pos, pos) == "-" then pos = pos + 1 end
        if s:sub(pos, pos) == "0" then
            pos = pos + 1
        else
            local _, e = s:find("^[1-9][0-9]*", pos)
            need(e, "invalid number"); pos = e + 1
        end
        local integral = true
        local _, e = s:find("^%.[0-9]+", pos)
        if e then pos = e + 1; integral = false else need(s:sub(pos, pos) ~= ".", "invalid number") end
        _, e = s:find("^[eE][+-]?[0-9]+", pos)
        if e then pos = e + 1; integral = false else need(not s:find("^[eE]", pos), "invalid number") end
        local text = s:sub(start, pos - 1)
        local v = tonumber(text)
        need(v ~= nil, "invalid number")
        local neg = text:sub(1, 1) == "-"
        local token = {tok = "num", neg = neg, f = v * 1.0} -- * keeps -0.0; + 0.0 would not
        if neg and v == 0 then token.f = -0.0 end
        if integral and math.type(v) == "integer" then token.int = v end
        return token
    end
    local function lit(word, v)
        need(s:sub(pos, pos + #word - 1) == word, "invalid literal")
        pos = pos + #word
        return v
    end
    function value()
        ws()
        local c = s:sub(pos, pos)
        if c == "{" then
            depth = depth + 1; need(depth <= MAX_DEPTH, "nesting too deep")
            pos = pos + 1
            local fields, count = {}, 0
            ws()
            if s:sub(pos, pos) == "}" then
                pos = pos + 1
            else
                while true do
                    ws()
                    need(s:sub(pos, pos) == '"', "expected object key")
                    local k = str()
                    need(fields[k] == nil, "duplicate field " .. k)
                    ws(); need(s:sub(pos, pos) == ":", "expected colon"); pos = pos + 1
                    fields[k] = value(); count = count + 1
                    ws()
                    local d = s:sub(pos, pos); pos = pos + 1
                    if d == "}" then break end
                    need(d == ",", "expected comma or end of object")
                end
            end
            depth = depth - 1
            return {tok = "obj", fields = fields, count = count}
        elseif c == "[" then
            depth = depth + 1; need(depth <= MAX_DEPTH, "nesting too deep")
            pos = pos + 1
            local items = {}
            ws()
            if s:sub(pos, pos) == "]" then
                pos = pos + 1
            else
                while true do
                    items[#items + 1] = value()
                    ws()
                    local d = s:sub(pos, pos); pos = pos + 1
                    if d == "]" then break end
                    need(d == ",", "expected comma or end of array")
                end
            end
            depth = depth - 1
            return {tok = "arr", items = items}
        elseif c == '"' then return str()
        elseif c == "-" or c:find("^[0-9]$") then return num()
        elseif c == "t" then return lit("true", true)
        elseif c == "f" then return lit("false", false)
        elseif c == "n" then return lit("null", M.null)
        end
        fail("expected value")
    end
    local v = value()
    ws()
    need(pos == #s + 1, "trailing characters")
    return v
end

---------------------------------------------------------------------------- schema
local function obj(v, what)
    need(type(v) == "table" and v.tok == "obj", what .. " must be an object")
    return v.fields
end
local function exact(fields, keys, optional, what)
    for k in pairs(fields) do need(keys[k] or (optional and optional[k]), "unknown field " .. k .. " in " .. what) end
    for k in pairs(keys) do need(fields[k] ~= nil, "missing field " .. k .. " in " .. what) end
end
local function uint(v, max, what)
    need(type(v) == "table" and v.tok == "num" and v.int ~= nil and not v.neg and v.int <= max, "invalid " .. what)
    return v.int
end
local function float(v, what)
    need(type(v) == "table" and v.tok == "num", "invalid " .. what)
    return v.f
end
local function text(v, what)
    need(type(v) == "string", "invalid " .. what)
    return v
end
local function point(v, what)
    need(type(v) == "table" and v.tok == "arr" and #v.items == 3, "invalid point " .. what)
    return {float(v.items[1], what), float(v.items[2], what), float(v.items[3], what)}
end
local OUTCOMES = {victory = true, defeat = true, fled = true}
local REJECTIONS = {not_your_turn = true, unavailable = true, no_path = true, cannot_aim = true, no_effect = true}
M.REJECTIONS = REJECTIONS
local ROLES = {wow = true, bg3 = true}
local TYPES = {}
TYPES.S = {dec = text}
TYPES.U32 = {dec = function(v, what) return uint(v, U32, what) end}
TYPES.POINT = {dec = point}
TYPES.OUTCOME = {dec = function(v, what)
    need(type(v) == "string" and OUTCOMES[v], "invalid " .. what); return v end}
TYPES.REJECTION = {dec = function(v, what)
    need(type(v) == "string" and REJECTIONS[v], "invalid " .. what); return v end}
TYPES.POINTS = {dec = function(v, what)
    need(type(v) == "table" and v.tok == "arr", "invalid " .. what)
    local out = {}
    for i, p in ipairs(v.items) do out[i] = point(p, what) end
    return out
end}
local BASIS = {"wow_origin", "POINT", "bridge_origin", "POINT", "yaw", "F64"}
local ACTOR = {"id", "S", "wow_guid", "S", "position", "POINT", "hp", "U32", "max_hp", "U32"}
TYPES.F64 = {dec = float}
local function record(layout, v, what)
    local fields = obj(v, what)
    local keys, out = {}, {}
    for i = 1, #layout, 2 do keys[layout[i]] = true end
    exact(fields, keys, nil, what)
    for i = 1, #layout, 2 do
        local name, kind = layout[i], layout[i + 1]
        out[name] = TYPES[kind].dec(fields[name], what .. "." .. name)
    end
    return out
end
TYPES.BASIS = {dec = function(v, what) return record(BASIS, v, what) end}
TYPES.ACTORS = {dec = function(v, what)
    need(type(v) == "table" and v.tok == "arr", "invalid " .. what)
    local out = {}
    for i, a in ipairs(v.items) do out[i] = record(ACTOR, a, what) end
    return out
end}

-- Field order matches serde's output order in wire.rs.
local SCHEMA = {
    begin = {"encounter", "S", "freeze_receipt", "S", "map", "U32", "basis", "BASIS", "actors", "ACTORS"},
    ready = {"encounter", "S"},
    turn = {"encounter", "S", "turn", "U32", "actor", "S"},
    move = {"encounter", "S", "turn", "U32", "action", "S", "actor", "S", "target", "POINT"},
    path = {"encounter", "S", "turn", "U32", "action", "S", "actor", "S", "points", "POINTS"},
    cast = {"encounter", "S", "turn", "U32", "action", "S", "actor", "S", "target", "S", "spell", "S"},
    effect = {"encounter", "S", "turn", "U32", "action", "S", "target", "S", "hp", "U32", "max_hp", "U32", "animation", "S"},
    end_turn = {"encounter", "S", "turn", "U32"},
    finished = {"encounter", "S", "outcome", "OUTCOME"},
    cleanup = {"encounter", "S"},
    cleaned = {"encounter", "S"},
    rejected = {"encounter", "S", "turn", "U32", "action", "S", "reason", "REJECTION", "detail", "S"},
    abort = {"encounter", "S", "reason", "S"},
    acted = {"encounter", "S", "turn", "U32", "action", "S", "actor", "S", "spell", "S", "target", "S", "points", "POINTS"},
    join = {"encounter", "S", "actors", "ACTORS"},
    roster = {"encounter", "S", "joins", "U32", "actors", "ACTORS"},
}
M.SCHEMA = SCHEMA
local DIRECTION = {begin = "wow", move = "wow", cast = "wow", end_turn = "wow", cleanup = "wow",
    ready = "bg3", turn = "bg3", path = "bg3", effect = "bg3", finished = "bg3", cleaned = "bg3", rejected = "bg3",
    acted = "bg3", join = "wow", roster = "bg3"}

local function decode_message(v)
    local fields = obj(v, "message")
    local kind = fields.type
    need(type(kind) == "string" and SCHEMA[kind], "unknown message type")
    local layout = SCHEMA[kind]
    local keys = {type = true}
    for i = 1, #layout, 2 do keys[layout[i]] = true end
    exact(fields, keys, nil, "message " .. kind)
    local out = {type = kind}
    for i = 1, #layout, 2 do
        local name = layout[i]
        out[name] = TYPES[layout[i + 1]].dec(fields[name], kind .. "." .. name)
    end
    return out
end

---------------------------------------------------------------------------- validation
local function id_ok(s)
    return type(s) == "string" and #s >= 1 and #s <= 64 and s:find("^[A-Za-z0-9_%-]+$") ~= nil
end
M.id_ok = id_ok
local function hex_ok(s, n)
    return type(s) == "string" and #s == n and s:find("^[0-9A-Fa-f]+$") ~= nil
end
local function finite(x) return type(x) == "number" and x == x and x ~= math.huge and x ~= -math.huge end
local function point_ok(p)
    if type(p) ~= "table" or #p ~= 3 then return false end
    for i = 1, 3 do
        if not finite(p[i]) or math.abs(p[i]) > 1000000.0 then return false end
    end
    return true
end
M.point_ok = point_ok
local function u32_ok(v) return math.type(v) == "integer" and v >= 0 and v <= U32 end

-- Actor lists (begin, join, roster): 1 to 64 unique ids, exact GUIDs, valid state.
local function actors_ok(actors)
    need(type(actors) == "table" and #actors >= 1 and #actors <= M.MAX_ACTORS, "invalid actor count")
    local seen = {}
    for _, a in ipairs(actors) do
        need(type(a) == "table" and id_ok(a.id) and not seen[a.id], "invalid or duplicate actor")
        seen[a.id] = true
        need(hex_ok(a.wow_guid, 16), "invalid WoW GUID")
        need(point_ok(a.position) and u32_ok(a.max_hp) and u32_ok(a.hp) and a.max_hp > 0 and a.hp <= a.max_hp,
            "invalid actor state")
    end
end

-- Same rules as Message::validate in wire.rs, plus Lua type checks serde gets from its types.
local function validate(m, sender)
    need(type(m) == "table" and SCHEMA[m.type], "unknown message type")
    need(id_ok(m.encounter), "invalid encounter")
    local direction = DIRECTION[m.type]
    if m.type == "abort" then
        need(type(m.reason) == "string" and #m.reason >= 1 and #m.reason <= 256 and utf8_ok(m.reason), "invalid abort reason")
        direction = sender
    end
    need(direction == sender, "wrong message direction")
    local t = m.type
    if t == "begin" then
        need(id_ok(m.freeze_receipt) and type(m.basis) == "table" and point_ok(m.basis.wow_origin)
            and point_ok(m.basis.bridge_origin) and finite(m.basis.yaw) and u32_ok(m.map),
            "invalid freeze receipt or coordinate basis")
        actors_ok(m.actors)
    elseif t == "join" then
        actors_ok(m.actors)
    elseif t == "roster" then
        need(u32_ok(m.joins), "invalid roster")
        actors_ok(m.actors)
    elseif t == "turn" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.actor), "invalid turn")
    elseif t == "move" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and id_ok(m.actor) and point_ok(m.target), "invalid move")
    elseif t == "path" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and id_ok(m.actor) and type(m.points) == "table"
            and #m.points >= 1 and #m.points <= M.MAX_PATH, "invalid path")
        for _, p in ipairs(m.points) do need(point_ok(p), "invalid path") end
    elseif t == "cast" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and id_ok(m.actor) and id_ok(m.target)
            and id_ok(m.spell), "invalid cast")
    elseif t == "effect" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and id_ok(m.target) and id_ok(m.animation)
            and u32_ok(m.hp) and u32_ok(m.max_hp) and m.max_hp > 0 and m.hp <= m.max_hp, "invalid effect")
    elseif t == "end_turn" then
        need(u32_ok(m.turn) and m.turn > 0, "invalid turn")
    elseif t == "finished" then
        need(OUTCOMES[m.outcome], "invalid outcome")
    elseif t == "rejected" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and REJECTIONS[m.reason]
            and type(m.detail) == "string" and #m.detail <= 256 and utf8_ok(m.detail), "invalid rejection")
    elseif t == "acted" then
        need(u32_ok(m.turn) and m.turn > 0 and id_ok(m.action) and id_ok(m.actor) and id_ok(m.spell)
            and id_ok(m.target) and type(m.points) == "table" and #m.points <= M.MAX_PATH,
            "invalid combatant action")
        for _, p in ipairs(m.points) do need(point_ok(p), "invalid combatant action") end
    end
end
function M.validate(m, sender) return guard(function() validate(m, sender); return true end) end

---------------------------------------------------------------------------- JSON encode
local ESCAPES = {['"'] = '\\"', ['\\'] = '\\\\', ['\n'] = '\\n', ['\r'] = '\\r', ['\t'] = '\\t',
    ['\b'] = '\\b', ['\f'] = '\\f'}
local function enc_str(s)
    need(type(s) == "string" and utf8_ok(s), "string is not valid UTF-8")
    return '"' .. (s:gsub('[\0-\31"\\]', function(c) return ESCAPES[c] or string.format("\\u%04x", c:byte()) end)) .. '"'
end
local function enc_uint(v, max)
    need(math.type(v) == "integer" and v >= 0 and v <= max, "integer out of range")
    return string.format("%d", v)
end
local function enc_float(x)
    need(finite(x), "non-finite number")
    x = x * 1.0
    for _, f in ipairs({"%.15g", "%.16g", "%.17g"}) do
        local s = string.format(f, x)
        if tonumber(s) == x then return s end
    end
    fail("float does not round-trip")
end
local function enc_point(p)
    need(type(p) == "table" and #p == 3, "invalid point")
    return "[" .. enc_float(p[1]) .. "," .. enc_float(p[2]) .. "," .. enc_float(p[3]) .. "]"
end
local ENC
local function enc_record(layout, v, extra)
    need(type(v) == "table", "expected table")
    local parts = extra and {extra} or {}
    for i = 1, #layout, 2 do
        parts[#parts + 1] = enc_str(layout[i]) .. ":" .. ENC[layout[i + 1]](v[layout[i]])
    end
    return "{" .. table.concat(parts, ",") .. "}"
end
ENC = {
    S = enc_str,
    U32 = function(v) return enc_uint(v, U32) end,
    F64 = enc_float,
    POINT = enc_point,
    OUTCOME = function(v) need(OUTCOMES[v], "invalid outcome"); return enc_str(v) end,
    REJECTION = function(v) need(REJECTIONS[v], "invalid rejection reason"); return enc_str(v) end,
    POINTS = function(v)
        need(type(v) == "table", "invalid points")
        local parts = {}
        for i, p in ipairs(v) do parts[i] = enc_point(p) end
        return "[" .. table.concat(parts, ",") .. "]"
    end,
    BASIS = function(v) return enc_record(BASIS, v) end,
    ACTORS = function(v)
        need(type(v) == "table", "invalid actors")
        local parts = {}
        for i, a in ipairs(v) do parts[i] = enc_record(ACTOR, a) end
        return "[" .. table.concat(parts, ",") .. "]"
    end,
}
local function enc_message(m)
    need(type(m) == "table" and SCHEMA[m.type], "unknown message type")
    return enc_record(SCHEMA[m.type], m, '"type":' .. enc_str(m.type))
end

local function check_frame(f)
    need(f.version == M.VERSION, "unsupported version")
    need(math.type(f.heartbeat) == "integer" and f.heartbeat > 0 and f.heartbeat <= M.MAX_SEQUENCE
        and math.type(f.seq) == "integer" and f.seq >= 0 and f.seq <= M.MAX_SEQUENCE
        and math.type(f.ack) == "integer" and f.ack >= 0 and f.ack <= M.MAX_SEQUENCE, "counter out of range")
    need((f.seq > 0) == (f.message ~= nil), "sequence/message mismatch")
    if f.message ~= nil then validate(f.message, f.sender) end
end

-- Encodes {version, session, sender, heartbeat, seq, ack, message|nil}.
local function encode(f)
    need(type(f) == "table" and ROLES[f.sender], "invalid frame")
    local body = "{" .. table.concat({
        '"version":' .. enc_uint(f.version, U16),
        '"session":' .. enc_str(f.session),
        '"sender":' .. enc_str(f.sender),
        '"heartbeat":' .. enc_uint(f.heartbeat, U64),
        '"seq":' .. enc_uint(f.seq, U64),
        '"ack":' .. enc_uint(f.ack, U64),
        '"message":' .. (f.message == nil and "null" or enc_message(f.message)),
    }, ",") .. "}"
    local bytes = '{"body":' .. enc_str(body) .. ',"bytes":' .. string.format("%d", #body)
        .. ',"checksum":' .. string.format("%d", checksum(body)) .. "}"
    need(#bytes <= M.MAX_FRAME, "frame too large")
    return bytes
end
function M.encode(f) return guard(encode, f) end

local function decode(bytes)
    need(type(bytes) == "string", "frame must be a string")
    need(#bytes <= M.MAX_FRAME, "frame too large")
    need(utf8_ok(bytes), "frame is not valid UTF-8")
    local env = obj(parse(bytes), "envelope")
    exact(env, {body = true, bytes = true, checksum = true}, nil, "envelope")
    local body = text(env.body, "body")
    local n = uint(env.bytes, U64, "byte count")
    local sum = uint(env.checksum, U32, "checksum")
    need(n == #body and sum == checksum(body), "incomplete or corrupt frame")
    local fields = obj(parse(body), "frame")
    exact(fields, {version = true, session = true, sender = true, heartbeat = true, seq = true, ack = true},
        {message = true}, "frame")
    local f = {
        version = uint(fields.version, U16, "version"),
        session = text(fields.session, "session"),
        sender = text(fields.sender, "sender"),
        heartbeat = uint(fields.heartbeat, U64, "heartbeat"),
        seq = uint(fields.seq, U64, "seq"),
        ack = uint(fields.ack, U64, "ack"),
    }
    need(ROLES[f.sender], "invalid sender")
    if fields.message ~= nil and fields.message ~= M.null then f.message = decode_message(fields.message) end
    check_frame(f)
    return f
end
function M.decode(bytes) return guard(decode, bytes) end

---------------------------------------------------------------------------- endpoint
local function equal(a, b)
    if type(a) ~= "table" or type(b) ~= "table" then return a == b end
    for k, v in pairs(a) do if not equal(v, b[k]) then return false end end
    for k in pairs(b) do if a[k] == nil then return false end end
    return true
end
M.equal = equal
local function copy(v)
    if type(v) ~= "table" then return v end
    local out = {}
    for k, x in pairs(v) do out[k] = copy(x) end
    return out
end
M.copy = copy
local OTHER = {wow = "bg3", bg3 = "wow"}

local Endpoint = {}
Endpoint.__index = Endpoint
-- One outstanding message per direction. Application validation precedes acknowledgement.
-- After expiry, reconcile the encounter and start a fresh session; never resume ambiguous effects.
function M.endpoint(session, role, now, timeout_ms)
    return guard(function()
        need(hex_ok(session, 32), "session must be 128-bit hex")
        need(ROLES[role], "invalid role")
        need(math.type(timeout_ms) == "integer" and timeout_ms > 0, "timeout must be positive")
        need(math.type(now) == "integer", "time must be integer milliseconds")
        return setmetatable({session = session, role = role, sent = 0, received = 0, heartbeat = 0,
            peer_heartbeat = 0, last_seen = now, timeout_ms = timeout_ms, failed = false}, Endpoint)
    end)
end
function Endpoint:busy() return self.pending ~= nil end
function Endpoint:expired() return self.failed end
local function tick(self, now)
    if now - self.last_seen > self.timeout_ms then self.failed = true end
    need(not self.failed, "link expired: reconcile encounter and use a fresh session")
end
function Endpoint:tick(now) return guard(function() tick(self, now); return true end) end
function Endpoint:offer(message)
    return guard(function()
        need(not self.failed and not self:busy() and self.sent < M.MAX_SEQUENCE, "endpoint busy, expired or exhausted")
        validate(message, self.role)
        local m = copy(message)
        encode({version = M.VERSION, session = self.session, sender = self.role, heartbeat = 1,
            seq = self.sent + 1, ack = self.received, message = m})
        self.sent = self.sent + 1
        self.pending = m
        return true
    end)
end
function Endpoint:frame()
    return guard(function()
        need(not self.failed and self.heartbeat < M.MAX_SEQUENCE, "endpoint expired or exhausted")
        self.heartbeat = self.heartbeat + 1
        return encode({version = M.VERSION, session = self.session, sender = self.role,
            heartbeat = self.heartbeat, seq = self:busy() and self.sent or 0, ack = self.received,
            message = self.pending})
    end)
end
-- apply(message) must validate fully and only then perform side effects. Return true to
-- accept; return nil/false plus a reason, or raise, to refuse without acknowledging.
function Endpoint:receive(bytes, now, apply)
    return guard(function()
        tick(self, now)
        local f = decode(bytes)
        need(f.session == self.session and f.sender == OTHER[self.role], "stale session or wrong peer")
        need(f.ack <= self.sent, "acknowledgement from the future")
        if f.heartbeat <= self.peer_heartbeat then return false end
        local delivered = false
        if f.message ~= nil then
            if f.seq == self.received + 1 then
                local ok, why = apply(copy(f.message))
                need(ok, why or "application refused message")
                self.received = f.seq
                self.last_message = f.message
                delivered = true
            else
                need(f.seq == self.received and equal(self.last_message, f.message), "sequence gap or conflicting retry")
            end
        end
        if f.ack == self.sent then self.pending = nil end
        self.peer_heartbeat = f.heartbeat
        self.last_seen = now
        return delivered
    end)
end

return M
