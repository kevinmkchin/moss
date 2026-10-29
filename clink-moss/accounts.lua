-- moss: a claude typed in a shell uses the Claude account its folder is for. accounts.txt (MOSS_ACCOUNTS_FILE, else
-- ~\.moss\accounts.txt) has "account <name> = <config folder>" and "folder <path> = <name>" lines. Before each prompt
-- line, CLAUDE_CONFIG_DIR becomes the config folder of the account the longest rule folder holding the current one
-- names; leaving it unsets it again, but only a value this script (or moss, for a tab it started under an account,
-- MOSS_ACCOUNT_DIR) set: one set by hand stays. The default account (Claude Code's own) is CLAUDE_CONFIG_DIR unset.
-- moss holds a claude that uses the wrong account anyway; this makes the right one the one that starts.
-- Shipped next to moss.exe and loaded through CLINK_PATH (not seeded into ~\.moss), so every moss has the current one.

local ours = os.getenv("MOSS_ACCOUNT_DIR")

local function accounts_file()
    local f = os.getenv("MOSS_ACCOUNTS_FILE")
    if f and f ~= "" then return f end
    return (os.getenv("USERPROFILE") or "") .. "\\.moss\\accounts.txt"
end

-- The config folder the current folder's account has: a path, "" for the default account (unset), nil when no rule
-- holds the folder (or its account isn't set up: claude starts as the default one, and moss holds it).
local function wanted_dir(cwd)
    local h = io.open(accounts_file(), "r")
    if not h then return nil end
    local dirs, rules = {}, {}
    for line in h:lines() do
        local kind, name, value = line:match("^%s*(%a+)%s+(.-)%s*=%s*(.-)%s*$")
        if kind == "account" then dirs[name:lower()] = value:gsub("[\\/]+$", "")
        elseif kind == "folder" then rules[#rules + 1] = { name, value } end
    end
    h:close()
    local c = cwd:lower():gsub("/", "\\")
    local best, best_len = nil, -1
    for _, r in ipairs(rules) do
        local f = r[1]:lower():gsub("/", "\\"):gsub("\\+$", "")
        if (c == f or c:sub(1, #f + 1) == f .. "\\") and #f > best_len then best, best_len = r[2]:lower(), #f end
    end
    if not best then return nil end
    if best == "default" then return "" end
    return dirs[best]
end

clink.onbeginedit(function()
    local want = wanted_dir(os.getcwd())
    local now = os.getenv("CLAUDE_CONFIG_DIR")
    if want and want ~= "" then
        if now ~= want then os.setenv("CLAUDE_CONFIG_DIR", want) end
        ours = want
    elseif ours and now == ours then
        -- out of the rule's folder (or in a folder for the default account): back to the default account
        os.setenv("CLAUDE_CONFIG_DIR", nil)
        ours = nil
    end
end)
