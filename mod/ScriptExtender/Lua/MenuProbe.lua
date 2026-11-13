-- Temporary, bounded first-run menu diagnostics. Never available in a running world.
local M = {}
function M.Start(runId)
    local sequence, ticks = 0, 0
    -- INDEX (optional): which of several nodes with this widget and name (BG3 keeps two copies of some dialogs, such as
    -- ModVerification), 1 for the first met; without it the target must be the only one.
    local function find(widget, name, index)
        local found, count = nil, 0
        local rows = {}
        local seen, visited = {}, 0
        local function walk(node, owner, depth)
            if not node or depth > 24 or visited >= 2048 or seen[tostring(node)] then return end
            seen[tostring(node)] = true
            visited = visited + 1
            local ok, nodeName = pcall(function() return node.Name end)
            if not ok then nodeName = nil end
            if node.Type == "ls.UIWidget" then owner = nodeName end
            rows[#rows + 1] = {Name = nodeName, Widget = owner, Type = node.Type}
            if owner == widget and nodeName == name then
                count = count + 1
                if index == nil or index == count then found = node end
            end
            for i = 1, math.min(node.VisualChildrenCount or 0, 128) do walk(node:VisualChild(i), owner, depth + 1) end
        end
        walk(Ext.UI.GetRoot(), nil, 0)
        if name == "@tree" then return rows end
        if index == nil then assert(count == 1, "Target must identify exactly one current menu node") end
        assert(found ~= nil, "No such menu node (" .. tostring(count) .. " with this widget and name)")
        return found
    end
    Ext.Events.Tick:Subscribe(function()
        ticks = ticks + 1
        if ticks % 15 ~= 0 then return end
        local raw = Ext.IO.LoadFile("wow-bg3/menu-command.json")
        if not raw or #raw > 8192 then return end
        local parsed, command = pcall(Ext.Json.Parse, raw)
        if not parsed or type(command) ~= "table" or command.RunId ~= runId then return end
        if math.type(command.Sequence) ~= "integer" or command.Sequence <= sequence or command.Sequence > 1024 then return end
        sequence = command.Sequence -- failed commands are never retried automatically
        local result = {RunId = runId, Sequence = sequence, Action = command.Action}
        local ok, value = pcall(function()
            if command.Action == "Tree" then return find(nil, "@tree") end
            local index = math.type(command.Index) == "integer" and command.Index or nil
            local target = find(command.Widget, command.Name, index)
            if command.Action == "Inspect" then
                local props = {}
                for key, prop in pairs(target:GetAllProperties()) do props[tostring(key)] = tostring(prop) end
                return props
            end
            if command.Action == "Activate" then
                assert(Ext.Utils.GetGameState() == "Menu", "Activation restricted to menus")
                assert(target.Type == "ls.LSButton", "Only a normal menu button may activate")
                if command.Widget == "Eula" then
                    assert(command.Name == "AcceptBtn" and Ext._Internal.Getenv("WOW_BG3_ACCEPT_EULA") == "1", "Licence acceptance not authorized")
                end
                local buttonCommand = target.Command
                assert(buttonCommand and buttonCommand:CanExecute(target.CommandParameter), "Button command unavailable or disabled")
                buttonCommand:Execute(target.CommandParameter)
                return "Command executed; verify actual UI transition"
            end
            if command.Action == "Key" then
                assert(Ext.Utils.GetGameState() == "Menu", "Input restricted to menus")
                local keys = {RETURN = true, ESCAPE = true, PAGEDOWN = true, END = true, SPACE = true, TAB = true}
                assert(keys[command.Key], "Unsupported setup key")
                Ext.Input.InjectKeyPress(command.Key)
                return "Input sent; verify actual UI transition"
            end
            error("Unknown menu action")
        end)
        result.OK, result.Value = ok, ok and value or tostring(value)
        Ext.IO.SaveFile("wow-bg3/menu-result.json", Ext.Json.Stringify(result))
    end)
end
return M
