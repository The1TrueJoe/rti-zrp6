-- Minimal JSON for the Remote Buttons tab (10_ui.lua). Pure Lua so it behaves the
-- same in the test harness as on a controller.

local ARRAY_MT = {__jsontype = 'array'}

-- Marks a table so that it encodes as [] even when empty.
function JSON.Array(t)
	return setmetatable(t or {}, ARRAY_MT)
end

local function EscapeString(s)
	s = string.gsub(s, '[%c"\\]', function(c)
		if (c == '"') then return '\\"' end
		if (c == '\\') then return '\\\\' end
		if (c == '\n') then return '\\n' end
		if (c == '\r') then return '\\r' end
		if (c == '\t') then return '\\t' end
		return string.format('\\u%04x', string.byte(c))
	end)
	return '"' .. s .. '"'
end

local function IsArray(t)
	if (getmetatable(t) == ARRAY_MT) then
		return true
	end
	local n = 0
	for k, _ in pairs(t) do
		if (type(k) ~= 'number' or k < 1 or k ~= math.floor(k)) then
			return false
		end
		n = n + 1
	end
	return n > 0 and n == #t
end

local function Encode(value, out)
	local kind = type(value)
	if (value == nil) then
		table.insert(out, 'null')
	elseif (kind == 'boolean') then
		table.insert(out, value and 'true' or 'false')
	elseif (kind == 'number') then
		if (value ~= value or value == math.huge or value == -math.huge) then
			table.insert(out, 'null')
		elseif (value == math.floor(value) and math.abs(value) < 1e15) then
			table.insert(out, string.format('%d', value))
		else
			table.insert(out, string.format('%.14g', value))
		end
	elseif (kind == 'string') then
		table.insert(out, EscapeString(value))
	elseif (kind == 'table') then
		if (IsArray(value)) then
			table.insert(out, '[')
			for i = 1, #value do
				if (i > 1) then table.insert(out, ',') end
				Encode(value[i], out)
			end
			table.insert(out, ']')
		else
			local keys = {}
			for k, _ in pairs(value) do
				table.insert(keys, tostring(k))
			end
			table.sort(keys)
			table.insert(out, '{')
			for i, k in ipairs(keys) do
				if (i > 1) then table.insert(out, ',') end
				table.insert(out, EscapeString(k))
				table.insert(out, ':')
				local v = value[k]
				if (v == nil) then v = value[tonumber(k)] end
				Encode(v, out)
			end
			table.insert(out, '}')
		end
	else
		table.insert(out, 'null')
	end
end

function JSON.Encode(value)
	local out = {}
	Encode(value, out)
	return table.concat(out)
end

-- Decoder -------------------------------------------------------------------

local function SkipWs(s, i)
	local _, e = string.find(s, '^[ \t\r\n]*', i)
	return e + 1
end

local DecodeValue

local function Utf8(code)
	if (code < 0x80) then
		return string.char(code)
	elseif (code < 0x800) then
		return string.char(0xC0 + math.floor(code / 0x40), 0x80 + (code % 0x40))
	else
		return string.char(0xE0 + math.floor(code / 0x1000), 0x80 + (math.floor(code / 0x40) % 0x40), 0x80 + (code % 0x40))
	end
end

local function DecodeString(s, i)
	-- s:sub(i, i) == '"'
	local out = {}
	i = i + 1
	while (true) do
		local c = string.sub(s, i, i)
		if (c == '') then
			error('unterminated string')
		elseif (c == '"') then
			return table.concat(out), i + 1
		elseif (c == '\\') then
			local e = string.sub(s, i + 1, i + 1)
			if (e == 'n') then table.insert(out, '\n')
			elseif (e == 'r') then table.insert(out, '\r')
			elseif (e == 't') then table.insert(out, '\t')
			elseif (e == 'b') then table.insert(out, '\b')
			elseif (e == 'f') then table.insert(out, '\f')
			elseif (e == 'u') then
				local hex = string.sub(s, i + 2, i + 5)
				if (not string.match(hex, '^%x%x%x%x$')) then error('bad \\u escape') end
				table.insert(out, Utf8(tonumber(hex, 16)))
				i = i + 4
			else
				table.insert(out, e)
			end
			i = i + 2
		else
			local j = string.find(s, '["\\]', i)
			if (not j) then error('unterminated string') end
			table.insert(out, string.sub(s, i, j - 1))
			i = j
		end
	end
end

DecodeValue = function(s, i)
	i = SkipWs(s, i)
	local c = string.sub(s, i, i)
	if (c == '{') then
		local obj = {}
		i = SkipWs(s, i + 1)
		if (string.sub(s, i, i) == '}') then
			return obj, i + 1
		end
		while (true) do
			i = SkipWs(s, i)
			if (string.sub(s, i, i) ~= '"') then error('expected key at ' .. i) end
			local key
			key, i = DecodeString(s, i)
			i = SkipWs(s, i)
			if (string.sub(s, i, i) ~= ':') then error('expected : at ' .. i) end
			local value
			value, i = DecodeValue(s, i + 1)
			obj[key] = value
			i = SkipWs(s, i)
			local d = string.sub(s, i, i)
			if (d == '}') then return obj, i + 1 end
			if (d ~= ',') then error('expected , or } at ' .. i) end
			i = i + 1
		end
	elseif (c == '[') then
		local arr = JSON.Array({})
		i = SkipWs(s, i + 1)
		if (string.sub(s, i, i) == ']') then
			return arr, i + 1
		end
		while (true) do
			local value
			value, i = DecodeValue(s, i)
			table.insert(arr, value)
			i = SkipWs(s, i)
			local d = string.sub(s, i, i)
			if (d == ']') then return arr, i + 1 end
			if (d ~= ',') then error('expected , or ] at ' .. i) end
			i = i + 1
		end
	elseif (c == '"') then
		return DecodeString(s, i)
	elseif (string.sub(s, i, i + 3) == 'true') then
		return true, i + 4
	elseif (string.sub(s, i, i + 4) == 'false') then
		return false, i + 5
	elseif (string.sub(s, i, i + 3) == 'null') then
		return nil, i + 4
	else
		local num = string.match(s, '^-?%d+%.?%d*[eE]?[-+]?%d*', i)
		if (not num or num == '') then error('unexpected character at ' .. i) end
		return tonumber(num), i + string.len(num)
	end
end

-- Returns value or nil, error.
function JSON.Decode(s)
	if (type(s) ~= 'string' or s == '') then
		return nil, 'empty'
	end
	local ok, value, pos = pcall(DecodeValue, s, 1)
	if (not ok) then
		return nil, value
	end
	return value
end
