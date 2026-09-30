-- General helpers. Every call into C4 goes through U so a missing API (an old
-- OS, or the test harness) degrades to a log line instead of a Lua error.

function U.Trim(value)
	value = tostring(value or '')
	return (string.gsub(value, '^%s*(.-)%s*$', '%1'))
end

function U.Split(value, sep)
	local out = {}
	value = tostring(value or '')
	local pattern = '([^' .. sep .. ']+)'
	for item in string.gmatch(value, pattern) do
		table.insert(out, item)
	end
	return out
end

function U.SafeCall(fn, ...)
	local ok, result = pcall(fn, ...)
	if (ok) then
		return result
	end
	if (STATE.debug) then
		print(LOG_PREFIX .. 'error: ' .. tostring(result))
	end
	return nil
end

function U.Log(message)
	print(LOG_PREFIX .. tostring(message))
end

function U.Debug(message)
	if (STATE.debug) then
		print(LOG_PREFIX .. tostring(message))
	end
end

-- Printable form of arbitrary bytes for logs: \r, \n, \xHH.
function U.Printable(data, max)
	data = tostring(data or '')
	max = max or 120
	local out = string.gsub(data, '[^%w%p ]', function(c)
		if (c == '\r') then return '\\r' end
		if (c == '\n') then return '\\n' end
		return string.format('\\x%02X', string.byte(c))
	end)
	if (string.len(out) > max) then
		out = string.sub(out, 1, max) .. '...'
	end
	return out
end

-- Decodes the escapes an installer can type into a Composer STRING field:
-- \r \n \t \\ \xHH. Used by the Send Serial programming command.
function U.DecodeEscapes(value)
	value = tostring(value or '')
	local out = {}
	local i = 1
	local n = string.len(value)
	while (i <= n) do
		local c = string.sub(value, i, i)
		if (c == '\\' and i < n) then
			local d = string.sub(value, i + 1, i + 1)
			if (d == 'r') then table.insert(out, '\r'); i = i + 2
			elseif (d == 'n') then table.insert(out, '\n'); i = i + 2
			elseif (d == 't') then table.insert(out, '\t'); i = i + 2
			elseif (d == '\\') then table.insert(out, '\\'); i = i + 2
			elseif (d == 'x' and string.match(string.sub(value, i + 2, i + 3), '^%x%x$')) then
				table.insert(out, string.char(tonumber(string.sub(value, i + 2, i + 3), 16)))
				i = i + 4
			else
				table.insert(out, c); i = i + 1
			end
		else
			table.insert(out, c); i = i + 1
		end
	end
	return table.concat(out)
end

function U.XmlUnescape(value)
	value = tostring(value or '')
	value = string.gsub(value, '&lt;', '<')
	value = string.gsub(value, '&gt;', '>')
	value = string.gsub(value, '&quot;', '"')
	value = string.gsub(value, '&apos;', "'")
	value = string.gsub(value, '&amp;', '&')
	return value
end

function U.IsTrue(value)
	if (value == true) then return true end
	local v = string.lower(U.Trim(value))
	return (v == '1' or v == 'true' or v == 'on' or v == 'yes')
end

-- Milliseconds from a monotonic-ish clock.
function U.Now()
	if (C4 and C4.GetTickCount) then
		local t = U.SafeCall(function() return C4:GetTickCount() end)
		if (t) then return t end
	end
	return math.floor(os.clock() * 1000)
end

-- Returns a C4LuaTimer (with :Cancel()) or nil.
function U.SetTimer(ms, fn, repeating)
	if (not (C4 and C4.SetTimer)) then
		return nil
	end
	return U.SafeCall(function()
		return C4:SetTimer(ms, function(timer, skips)
			local ok, err = pcall(fn, timer, skips)
			if (not ok) then
				print(LOG_PREFIX .. 'timer error: ' .. tostring(err))
			end
		end, repeating == true)
	end)
end

function U.CancelTimer(timer)
	if (timer and timer.Cancel) then
		U.SafeCall(function() timer:Cancel() end)
	end
	return nil
end

function U.UpdateProperty(name, value)
	if (C4 and C4.UpdateProperty) then
		U.SafeCall(function() C4:UpdateProperty(name, tostring(value == nil and '' or value)) end)
	end
end

function U.PropertyVisible(name, visible)
	if (C4 and C4.SetPropertyAttribs) then
		U.SafeCall(function() C4:SetPropertyAttribs(name, visible and 0 or 1) end)
	end
end

function U.SetVariable(name, value)
	if (C4 and C4.SetVariable) then
		U.SafeCall(function() C4:SetVariable(name, tostring(value == nil and '' or value)) end)
	end
end

function U.FireEvent(name)
	if (C4 and C4.FireEvent) then
		U.SafeCall(function() C4:FireEvent(name) end)
	end
end

function U.SendToProxy(binding, command, params)
	if (C4 and C4.SendToProxy) then
		U.SafeCall(function() C4:SendToProxy(binding, command, params or {}, 'NOTIFY') end)
	end
end

function U.PersistGet(name)
	if (C4 and C4.PersistGetValue) then
		return U.SafeCall(function() return C4:PersistGetValue(name) end)
	end
	return nil
end

function U.PersistSet(name, value)
	if (C4 and C4.PersistSetValue) then
		U.SafeCall(function() C4:PersistSetValue(name, value) end)
	end
end

function U.Prop(name, default)
	local value = Properties and Properties[name]
	if (value == nil) then
		return default
	end
	return value
end

function U.DeviceId()
	if (C4 and C4.GetDeviceID) then
		return U.SafeCall(function() return C4:GetDeviceID() end)
	end
	return nil
end

function U.Contains(list, value)
	for _, item in ipairs(list) do
		if (item == value) then return true end
	end
	return false
end

function U.SetLastError(message)
	STATE.lastError = message
	U.Log(message)
	U.UpdateProperty('Last Error', message)
end
