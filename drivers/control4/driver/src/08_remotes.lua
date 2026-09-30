-- Learned RTI remote buttons and what they do.
--
-- The box publishes every 433 MHz button press on remote/code. The bench
-- remote's codes are bare ("14.0.2.8.0.2.9"); any other remote's are
-- "p<preamble>:<button>" ("p16.16.14.12.15:14.0.2.8"), so the preamble
-- identifies the remote ("p16.16.14.12.15", or "" for the bench remote) and
-- the whole code identifies the button. Codes are static, so a learned
-- button stays learned.
--
-- Each learned button can run any number of room actions (07_rooms.lua) and
-- always fires two programming events: "Remote Button Pressed" and its own
-- dynamic event, so anything Composer programming can do is reachable too.
-- Everything is kept with C4:PersistSetValue.

REMOTES.data = {remotes = {}, buttons = {}, nextEvent = ZRP.EVENT_DYNAMIC_BASE}
REMOTES.learn = {armed = false, name = '', expires = 0, captured = nil}
REMOTES.recent = {}
REMOTES.DEFAULT_ID = ''

-- Returns remoteId, buttonPart.
function REMOTES.Parse(code)
	code = U.Trim(code)
	local pre, button = string.match(code, '^(p[^:]+):(.+)$')
	if (pre) then
		return pre, button
	end
	return REMOTES.DEFAULT_ID, code
end

function REMOTES.CleanName(name, fallback)
	name = U.Trim(string.gsub(tostring(name or ''), '[%c,]', ' '))
	name = string.gsub(name, '%s+', ' ')
	if (string.len(name) > 40) then
		name = string.sub(name, 1, 40)
	end
	if (name == '') then
		return fallback
	end
	return name
end

-- Stored with remotes as a list: the bench remote's id is "", which is not
-- a table key worth trusting to Director's persistence.
function REMOTES.Save()
	local remotes = {}
	for id, r in pairs(REMOTES.data.remotes) do
		table.insert(remotes, {id = id, name = r.name})
	end
	U.PersistSet(ZRP.PERSIST_REMOTES, {
		version = 2,
		remotes = remotes,
		buttons = REMOTES.data.buttons,
		nextEvent = REMOTES.data.nextEvent,
	})
end

function REMOTES.Load()
	local data = U.PersistGet(ZRP.PERSIST_REMOTES)
	if (type(data) ~= 'table') then
		return
	end
	local remotes = {}
	for k, r in pairs(type(data.remotes) == 'table' and data.remotes or {}) do
		if (type(r) == 'table') then
			local id = tostring(r.id or k)
			if (id == 'default') then id = '' end -- 1.0.0 called the bench remote "default"
			remotes[id] = {id = id, name = tostring(r.name or 'Remote')}
		end
	end
	local buttons = {}
	for code, btn in pairs(type(data.buttons) == 'table' and data.buttons or {}) do
		if (type(btn) == 'table') then
			btn.code = tostring(btn.code or code)
			btn.remote = REMOTES.Parse(btn.code)
			local actions = {}
			for _, act in ipairs(type(btn.actions) == 'table' and btn.actions or {}) do
				local clean = ROOMS.Normalize(act) -- also turns 1.0.0 action ids into commands
				if (clean) then table.insert(actions, clean) end
			end
			btn.actions = actions
			buttons[btn.code] = btn
			if (not remotes[btn.remote]) then
				remotes[btn.remote] = {id = btn.remote, name = 'Remote'}
			end
		end
	end
	REMOTES.data = {
		remotes = remotes,
		buttons = buttons,
		nextEvent = tonumber(data.nextEvent) or ZRP.EVENT_DYNAMIC_BASE,
	}
end

local function CountKeys(t)
	local n = 0
	for _ in pairs(t) do n = n + 1 end
	return n
end

function REMOTES.EnsureRemote(id)
	local remotes = REMOTES.data.remotes
	if (not remotes[id]) then
		remotes[id] = {id = id, name = 'Remote ' .. (CountKeys(remotes) + 1)}
	end
	return remotes[id]
end

function REMOTES.RemoteName(id)
	local r = REMOTES.data.remotes[id]
	return r and r.name or tostring(id)
end

function REMOTES.EventName(btn)
	return 'Button: ' .. REMOTES.RemoteName(btn.remote) .. ' / ' .. btn.name
end

function REMOTES.Label(btn)
	return REMOTES.RemoteName(btn.remote) .. ' / ' .. btn.name
end

function REMOTES.RegisterEvent(btn)
	if (C4 and C4.AddEvent and btn.event) then
		U.SafeCall(function()
			C4:AddEvent(btn.event, REMOTES.EventName(btn), 'When the NAME ' .. REMOTES.Label(btn) .. ' button is pressed')
		end)
	end
end

function REMOTES.RegisterAllEvents()
	for _, btn in pairs(REMOTES.data.buttons) do
		REMOTES.RegisterEvent(btn)
	end
end

local function RegisterRemoteEvents(remoteId)
	for _, btn in pairs(REMOTES.data.buttons) do
		if (btn.remote == remoteId) then
			REMOTES.RegisterEvent(btn)
		end
	end
end

function REMOTES.ButtonsOf(remoteId)
	local list = {}
	for _, btn in pairs(REMOTES.data.buttons) do
		if (btn.remote == remoteId) then
			table.insert(list, btn)
		end
	end
	return list
end

-- Adds a button (or renames an existing one). Returns btn, created.
function REMOTES.Learn(code, name)
	code = U.Trim(code)
	if (code == '') then
		return nil, false
	end
	local buttons = REMOTES.data.buttons
	local btn = buttons[code]
	local created = false
	if (not btn) then
		local remoteId = REMOTES.Parse(code)
		REMOTES.EnsureRemote(remoteId)
		btn = {
			code = code,
			remote = remoteId,
			name = REMOTES.CleanName(name, 'Button ' .. (#REMOTES.ButtonsOf(remoteId) + 1)),
			event = REMOTES.data.nextEvent,
			actions = {},
		}
		REMOTES.data.nextEvent = REMOTES.data.nextEvent + 1
		buttons[code] = btn
		created = true
	elseif (U.Trim(name) ~= '') then
		btn.name = REMOTES.CleanName(name, btn.name)
	end
	REMOTES.RegisterEvent(btn)
	REMOTES.Save()
	REMOTES.Changed()
	return btn, created
end

function REMOTES.CleanAction(a)
	return ROOMS.Normalize(a)
end

-- fields: {code, name, actions = {...}}. Creates the button if needed.
function REMOTES.Upsert(fields)
	local code = U.Trim(fields.code)
	if (code == '') then
		return nil, 'code is required'
	end
	local btn = REMOTES.data.buttons[code]
	if (not btn) then
		btn = REMOTES.Learn(code, fields.name)
	elseif (fields.name ~= nil) then
		btn.name = REMOTES.CleanName(fields.name, btn.name)
	end
	if (type(fields.actions) == 'table') then
		local actions = {}
		for _, a in ipairs(fields.actions) do
			local clean = REMOTES.CleanAction(a)
			if (clean) then
				table.insert(actions, clean)
			end
		end
		btn.actions = actions
	end
	REMOTES.RegisterEvent(btn)
	REMOTES.Save()
	REMOTES.Changed()
	return btn
end

function REMOTES.Delete(code)
	local btn = REMOTES.data.buttons[code]
	if (not btn) then
		return false
	end
	REMOTES.data.buttons[code] = nil
	if (C4 and C4.DeleteEvent and btn.event) then
		U.SafeCall(function() C4:DeleteEvent(btn.event) end)
	end
	-- Forget a remote with no buttons left.
	if (#REMOTES.ButtonsOf(btn.remote) == 0) then
		REMOTES.data.remotes[btn.remote] = nil
	end
	REMOTES.Save()
	REMOTES.Changed()
	return true
end

function REMOTES.RenameRemote(id, name)
	local r = REMOTES.data.remotes[id]
	if (not r) then
		return false
	end
	r.name = REMOTES.CleanName(name, r.name)
	RegisterRemoteEvents(id)
	REMOTES.Save()
	REMOTES.Changed()
	return true
end

function REMOTES.Execute(btn)
	local errors = {}
	for i, action in ipairs(btn.actions or {}) do
		local ok, err = ROOMS.Run(action)
		if (not ok) then
			table.insert(errors, 'action ' .. i .. ': ' .. tostring(err))
		end
	end
	if (#errors > 0) then
		U.SetLastError(REMOTES.Label(btn) .. ': ' .. table.concat(errors, '; '))
	end
	return #errors == 0
end

-- Learn ------------------------------------------------------------------------------

function REMOTES.Arm(name, seconds)
	REMOTES.learn.armed = true
	REMOTES.learn.name = REMOTES.CleanName(name, '')
	REMOTES.learn.expires = U.Now() + (tonumber(seconds) or ZRP.LEARN_TIMEOUT_S) * 1000
	REMOTES.learn.captured = nil
	UI.PushState()
end

function REMOTES.Disarm(status)
	REMOTES.learn.armed = false
	UI.PushState()
end

function REMOTES.Tick()
	if (REMOTES.learn.armed and U.Now() > REMOTES.learn.expires) then
		REMOTES.Disarm('Timed out; no button pressed')
	end
end

-- A code arrived from the box.
function REMOTES.OnCode(code)
	code = U.Trim(code)
	if (code == '') then
		return
	end
	local btn = REMOTES.data.buttons[code]
	table.insert(REMOTES.recent, 1, {code = code, at = U.Now(), known = btn ~= nil})
	while (#REMOTES.recent > 20) do
		table.remove(REMOTES.recent)
	end
	U.UpdateProperty('Last Remote Code', code)
	U.SetVariable('LAST_REMOTE_CODE', code)

	if (REMOTES.learn.armed) then
		local learned, created = REMOTES.Learn(code, REMOTES.learn.name)
		REMOTES.learn.captured = code
		REMOTES.Disarm((created and 'Learned ' or 'Already known: ') .. REMOTES.Label(learned))
		U.Log('learned ' .. code .. ' as ' .. REMOTES.Label(learned))
		return
	end

	if (btn) then
		U.SetVariable('LAST_REMOTE_BUTTON', btn.name)
		U.SetVariable('LAST_REMOTE_NAME', REMOTES.RemoteName(btn.remote))
		U.FireEvent('Remote Button Pressed')
		U.FireEvent(REMOTES.EventName(btn))
		U.Debug('button ' .. REMOTES.Label(btn))
		REMOTES.Execute(btn)
	else
		local remoteId = REMOTES.Parse(code)
		U.SetVariable('LAST_REMOTE_BUTTON', '')
		U.SetVariable('LAST_REMOTE_NAME', REMOTES.data.remotes[remoteId] and REMOTES.RemoteName(remoteId) or '')
		U.FireEvent('Remote Button Pressed')
		U.FireEvent('Unmapped Remote Button Pressed')
	end
	UI.PushState() -- the tab's "Recent presses"
end

-- Views --------------------------------------------------------------------------------

function REMOTES.SortedButtons()
	local list = {}
	for _, btn in pairs(REMOTES.data.buttons) do
		table.insert(list, btn)
	end
	table.sort(list, function(a, b)
		local ra, rb = string.lower(REMOTES.RemoteName(a.remote)), string.lower(REMOTES.RemoteName(b.remote))
		if (ra ~= rb) then return ra < rb end
		return (a.event or 0) < (b.event or 0)
	end)
	return list
end

-- Called whenever buttons or remotes change: refreshes the Remote Buttons tab.
function REMOTES.Changed()
	UI.PushState()
end
