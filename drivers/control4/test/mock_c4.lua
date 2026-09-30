-- A mock of the DriverWorks environment: just enough of C4 for the driver
-- to run under plain Lua / LuaJIT, with virtual time and a record of every
-- side effect so tests can assert on them.
--
-- Runs under Lua 5.1 (LuaJIT) and 5.2+.

local M = {}

-- base64, for C4:Base64Encode/Decode
local B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/'
function M.b64encode(data)
	return ((data:gsub('.', function(x)
		local r, b = '', x:byte()
		for i = 8, 1, -1 do r = r .. (b % 2 ^ i - b % 2 ^ (i - 1) > 0 and '1' or '0') end
		return r
	end) .. '0000'):gsub('%d%d%d?%d?%d?%d?', function(x)
		if (#x < 6) then return '' end
		local c = 0
		for i = 1, 6 do c = c + (x:sub(i, i) == '1' and 2 ^ (6 - i) or 0) end
		return B64:sub(c + 1, c + 1)
	end) .. ({'', '==', '='})[#data % 3 + 1])
end
function M.b64decode(data)
	data = data:gsub('[^' .. B64 .. '=]', '')
	return (data:gsub('.', function(x)
		if (x == '=') then return '' end
		local r, f = '', (B64:find(x, 1, true) - 1)
		for i = 6, 1, -1 do r = r .. (f % 2 ^ i - f % 2 ^ (i - 1) > 0 and '1' or '0') end
		return r
	end):gsub('%d%d%d?%d?%d?%d?%d?%d?', function(x)
		if (#x ~= 8) then return '' end
		local c = 0
		for i = 1, 8 do c = c + (x:sub(i, i) == '1' and 2 ^ (8 - i) or 0) end
		return string.char(c)
	end))
end

local function deepcopy(v)
	if (type(v) ~= 'table') then return v end
	local out = {}
	for k, x in pairs(v) do out[deepcopy(k)] = deepcopy(x) end
	return out
end
M.deepcopy = deepcopy

function M.new(opts)
	opts = opts or {}
	local C4 = {
		now = 100000,
		timers = {},
		log = {},
		props = {},          -- UpdateProperty values
		propAttribs = {},
		propLists = {},
		vars = {},
		varDefs = {},
		events = {added = {}, deleted = {}, fired = {}},
		proxy = {},          -- SendToProxy calls
		persist = deepcopy(opts.persist or {}),
		bindings = {},       -- dynamic bindings currently present [id] = {name, class}
		bindingLog = {},     -- {'add'|'remove', id, class}
		-- net.* holds binding 6001 (the MQTT connection) only; every other
		-- binding (the mDNS listener) goes to net.other.
		net = {created = {}, options = {}, connects = {}, disconnects = {}, sent = {}, other = {}},
		udpServers = {},     -- C4:CreateServer calls
		serverResult = (opts.serverResult == nil) and true or opts.serverResult,
		deviceCalls = {},    -- SendToDevice calls
		sources = opts.sources or {},      -- [roomId] = {watch = {ids}, listen = {ids}}
		driverConfig = opts.driverConfig or {}, -- [deviceId] = xml
		dataToUI = {},       -- SendDataToUI calls {command, params}
		deviceId = opts.deviceId or 4242,
	}

	-- time / timers
	function C4:GetTickCount() return self.now end
	function C4:SetTimer(ms, fn, rep)
		assert(ms > 0, 'timer delay must be > 0')
		local t = {at = self.now + ms, ms = ms, fn = fn, rep = rep, cancelled = false}
		function t:Cancel() self.cancelled = true; return nil end
		table.insert(self.timers, t)
		return t
	end
	function C4:Advance(ms)
		local target = self.now + ms
		while (true) do
			local best = nil
			for _, t in ipairs(self.timers) do
				if (not t.cancelled and t.at <= target and (best == nil or t.at < best.at)) then
					best = t
				end
			end
			if (not best) then break end
			self.now = best.at
			if (best.rep) then
				best.at = best.at + best.ms
			else
				best.cancelled = true
			end
			best.fn(best, 0)
		end
		self.now = target
		local live = {}
		for _, t in ipairs(self.timers) do
			if (not t.cancelled) then table.insert(live, t) end
		end
		self.timers = live
	end

	-- properties / variables / events
	function C4:UpdateProperty(name, value) self.props[name] = value end
	function C4:SetPropertyAttribs(name, v) self.propAttribs[name] = v end
	function C4:UpdatePropertyList(name, list, default) self.propLists[name] = {list = list, default = default} end
	function C4:AddVariable(name, value, kind) self.varDefs[name] = kind; self.vars[name] = value; return 1, true end
	function C4:SetVariable(name, value) self.vars[name] = value end
	function C4:AddEvent(id, name, desc) self.events.added[id] = name; return true end
	function C4:DeleteEvent(id) self.events.added[id] = nil; table.insert(self.events.deleted, id) end
	function C4:FireEvent(name) table.insert(self.events.fired, name) end
	function C4:SendToProxy(binding, command, params, kind)
		table.insert(self.proxy, {binding = binding, command = command, params = params, kind = kind})
	end
	function C4:PersistGetValue(name) return deepcopy(self.persist[name]) end
	function C4:PersistSetValue(name, value) self.persist[name] = deepcopy(value) end
	function C4:GetDeviceID() return self.deviceId end

	-- bindings
	function C4:AddDynamicBinding(id, kind, provider, name, class, hidden, autobind)
		self.bindings[id] = {name = name, class = class, provider = provider, kind = kind}
		table.insert(self.bindingLog, {'add', id, class, name})
	end
	function C4:RemoveDynamicBinding(id)
		self.bindings[id] = nil
		table.insert(self.bindingLog, {'remove', id})
	end
	function C4:GetDriverConfigInfoById(id, tag)
		return self.driverConfig[tonumber(id)]
	end

	-- network
	local function netlog(self, binding, what, entry)
		entry.binding = binding
		if (binding == 6001) then
			table.insert(self.net[what], entry)
		else
			entry.what = what
			table.insert(self.net.other, entry)
		end
	end
	function C4:CreateNetworkConnection(binding, address, kind)
		netlog(self, binding, 'created', {address = address, kind = kind})
	end
	function C4:NetPortOptions(binding, port, kind, options)
		netlog(self, binding, 'options', {port = port, kind = kind, options = options})
	end
	function C4:NetConnect(binding, port, kind)
		netlog(self, binding, 'connects', {port = port, kind = kind, at = self.now})
	end
	function C4:NetDisconnect(binding, port)
		netlog(self, binding, 'disconnects', {port = port, at = self.now})
	end
	function C4:CreateServer(port, delimiter, udp, identifier)
		table.insert(self.udpServers, {port = port, delimiter = delimiter, udp = udp, identifier = identifier, live = true})
		if (self.serverResult) then return true end
		return false, 'Address already in use'
	end
	function C4:DestroyServer(port)
		for _, s in ipairs(self.udpServers) do
			if (s.port == port) then s.live = false end
		end
	end
	function C4:SendToNetwork(binding, port, data)
		table.insert(self.net.sent, {binding = binding, port = port, data = data, at = self.now})
	end

	-- rooms
	function C4:SendToDevice(id, command, params)
		table.insert(self.deviceCalls, {id = id, command = command, params = params})
		return nil
	end
	-- A room answers GET_WATCH_DEVICES / GET_LISTEN_DEVICES as a UI request
	-- with <source> entries (junohouse/control4-adapter FINDINGS.md, section 1).
	function C4:SendUIRequest(id, command, params)
		local room = self.sources[id]
		if (room and (command == 'GET_WATCH_DEVICES' or command == 'GET_LISTEN_DEVICES')) then
			local ids = (command == 'GET_WATCH_DEVICES') and room.watch or room.listen
			local parts = {'<devices>'}
			for _, sid in ipairs(ids or {}) do
				table.insert(parts, '<source><id>' .. sid .. '</id><type>6</type><name>Source ' .. sid .. '</name></source>')
			end
			table.insert(parts, '</devices>')
			return table.concat(parts)
		end
		return ''
	end
	-- Shape from the DriverWorks API reference (GetDevices, Example 1 Return).
	C4.devices = opts.devices or {}
	function C4:GetDevices(filter)
		return M.deepcopy(self.devices)
	end
	-- On a real OS 3.3.3 controller this gave nothing usable (junohouse agent/driver.lua).
	function C4:GetProjectHierarchy()
		return opts.hierarchy
	end
	function C4:ListGetDeviceName(id) return (opts.names or {})[id] end
	function C4:GetDeviceData(id, key) return nil end
	function C4:GetControllerNetworkAddress() return '192.168.1.10' end

	-- Composer tab bridge
	function C4:Base64Encode(s) return M.b64encode(s) end
	function C4:Base64Decode(s) return M.b64decode(s) end
	function C4:SendDataToUI(command, params)
		for _, v in pairs(params or {}) do
			assert(type(v) ~= 'boolean', 'SendDataToUI cannot carry booleans')
		end
		table.insert(self.dataToUI, {command = command, params = params})
	end

	return C4
end

-- Loads the built driver.lua into a fresh environment. Returns env, modules.
function M.load(path, C4, properties, printer)
	local f = assert(io.open(path, 'rb'))
	local src = f:read('*a')
	f:close()
	local modules = nil
	local env = setmetatable({
		C4 = C4,
		Properties = properties,
		print = printer or function(...) local t = {...}; for i = 1, select('#', ...) do t[i] = tostring(t[i]) end; table.insert(C4.log, table.concat(t, ' ')) end,
		ZRP6_TEST_HOOK = function(m) modules = m end,
	}, {__index = _G})
	local chunk
	if (setfenv) then
		chunk = assert(loadstring(src, '=driver.lua'))
		setfenv(chunk, env)
	else
		chunk = assert(load(src, '=driver.lua', 't', env))
	end
	chunk()
	return env, modules
end

return M
