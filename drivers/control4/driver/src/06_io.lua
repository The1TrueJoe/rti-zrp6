-- The box's I/O as Control4 connections, and the topics behind them.
--
--   IR 1-7        IR_OUT provider (dynamic binding 101-107), or RS_232
--                 provider (dynamic binding 111-117) when the port is
--                 switched to one-way serial in Properties
--   RS-232 1-2    RS_232 provider (201-202)
--   Relays 1-3    RELAY provider (301-303)
--   Triggers 1-3  RELAY provider (311-313) - 12 V outputs behave like relays
--   Contacts 1-3  CONTACT_SENSOR provider (401-403)
--
-- Relay and contact commands/notifications follow Control4's own templates
-- (drivers-template-code-public c4hooks/c4hook_relay.lua and
-- c4hook_sensor.lua): OPEN / CLOSE / TOGGLE / TRIGGER(TIME) / GET_STATE in;
-- OPENED / CLOSED on a change, STATE_OPENED / STATE_CLOSED for the initial
-- state.

IO.irConfig = {}      -- [n] = {mode = 'ir'|'serial', baud = 'auto'|number, polarity = 'rs232'|'ttl'}
IO.irBinding = {}     -- [n] = dynamic binding currently offered for IR port n
IO.boxIrMode = {}     -- [n] = what the box last reported on ir/n/mode
IO.serialConfig = {}  -- [n] = {baud = 'auto'|number, format = 'auto'|'8N1'...}
IO.boxSerialConfig = {}
IO.bound = {}         -- [binding] = {device = id, serial = {baud, format}}
IO.pending = {}       -- ['ir/1/mode'] = {value, at}
IO.outputs = {}       -- ['relay1'] = {on = bool, init = bool, pulse = timer}
IO.contacts = {}      -- [n] = {closed = bool, init = bool}
IO.unknownLogged = {} -- proxy commands already logged once
IO.serialRx = {}      -- [n] = {text, at}: the last data received on RS-232 n
IO.firmware = ''
IO.boxStatus = ''

for n = 1, ZRP.IR_PORTS do
	IO.irConfig[n] = {mode = 'ir', baud = 'auto', polarity = 'rs232'}
end
for n = 1, ZRP.SERIAL_PORTS do
	IO.serialConfig[n] = {baud = 'auto', format = 'auto'}
end

-- Bindings ---------------------------------------------------------------------

function IO.IrBindingId(n, mode)
	if (mode == 'serial') then
		return ZRP.BIND_IR_SERIAL_BASE + n
	end
	return ZRP.BIND_IR_BASE + n
end

-- Returns {kind, port} for one of our control bindings, or nil.
function IO.BindingInfo(id)
	id = tonumber(id)
	if (not id) then return nil end
	local function inRange(base, count)
		return id > base and id <= base + count
	end
	if (inRange(ZRP.BIND_IR_BASE, ZRP.IR_PORTS)) then
		return {kind = 'ir', port = id - ZRP.BIND_IR_BASE}
	elseif (inRange(ZRP.BIND_IR_SERIAL_BASE, ZRP.IR_PORTS)) then
		return {kind = 'irserial', port = id - ZRP.BIND_IR_SERIAL_BASE}
	elseif (inRange(ZRP.BIND_SERIAL_BASE, ZRP.SERIAL_PORTS)) then
		return {kind = 'serial', port = id - ZRP.BIND_SERIAL_BASE}
	elseif (inRange(ZRP.BIND_RELAY_BASE, ZRP.RELAYS)) then
		return {kind = 'relay', port = id - ZRP.BIND_RELAY_BASE}
	elseif (inRange(ZRP.BIND_TRIGGER_BASE, ZRP.TRIGGERS)) then
		return {kind = 'trigger', port = id - ZRP.BIND_TRIGGER_BASE}
	elseif (inRange(ZRP.BIND_CONTACT_BASE, ZRP.CONTACTS)) then
		return {kind = 'contact', port = id - ZRP.BIND_CONTACT_BASE}
	end
	return nil
end

local function IrBindingName(n, mode)
	if (mode == 'serial') then
		return 'IR ' .. n .. ' Serial Out (one-way)'
	end
	if (n == 7) then
		return 'IR Out 7 (HIGH OUT)'
	end
	return 'IR Out ' .. n
end

local function AddIrBinding(n, mode)
	local id = IO.IrBindingId(n, mode)
	local class = (mode == 'serial') and 'RS_232' or 'IR_OUT'
	if (C4 and C4.AddDynamicBinding) then
		U.SafeCall(function()
			C4:AddDynamicBinding(id, 'CONTROL', true, IrBindingName(n, mode), class, false, false)
		end)
	end
	IO.irBinding[n] = id
	return id
end

local function PersistIrBindings()
	local map = {}
	for n = 1, ZRP.IR_PORTS do
		map[tostring(n)] = IO.irBinding[n]
	end
	U.PersistSet(ZRP.PERSIST_IR_BINDINGS, map)
end

-- OnDriverInit: put back exactly the bindings that existed before, so
-- Director can restore the connections made to them in Composer.
function IO.RestoreIrBindings()
	local map = U.PersistGet(ZRP.PERSIST_IR_BINDINGS)
	if (type(map) ~= 'table') then
		return
	end
	for n = 1, ZRP.IR_PORTS do
		local id = tonumber(map[tostring(n)] or map[n])
		if (id) then
			local info = IO.BindingInfo(id)
			if (info and info.port == n) then
				AddIrBinding(n, info.kind == 'irserial' and 'serial' or 'ir')
			end
		end
	end
end

-- Makes each IR port offer the binding that matches its configured mode.
function IO.ApplyIrBindings()
	local changed = false
	for n = 1, ZRP.IR_PORTS do
		local want = IO.IrBindingId(n, IO.irConfig[n].mode)
		local have = IO.irBinding[n]
		if (have ~= want) then
			if (have and C4 and C4.RemoveDynamicBinding) then
				U.SafeCall(function() C4:RemoveDynamicBinding(have) end)
				IO.bound[have] = nil
			end
			AddIrBinding(n, IO.irConfig[n].mode)
			U.Log('IR ' .. n .. ' now offers ' .. (IO.irConfig[n].mode == 'serial' and 'an RS_232' or 'an IR_OUT') .. ' connection')
			changed = true
		end
	end
	if (changed) then
		PersistIrBindings()
	end
	return changed
end

-- Serial settings --------------------------------------------------------------

local PARITY = {none = 'N', n = 'N', odd = 'O', o = 'O', even = 'E', e = 'E'}

-- Parses a Control4 <serialsettings> string ("9600 8 odd 1 none") into
-- {baud = 9600, format = '8O1'}. Returns nil if it cannot.
function IO.ParseSerialSettings(text)
	local tokens = U.Split(string.lower(U.Trim(text)), '%s')
	local baud = tonumber(tokens[1])
	if (not baud) then
		return nil
	end
	local bits = tonumber(tokens[2]) or 8
	local parity = PARITY[tokens[3] or 'none'] or 'N'
	local stop = tonumber(tokens[4]) or 1
	return {baud = baud, format = tostring(bits) .. parity .. tostring(stop)}
end

-- Reads the bound device driver's <serialsettings>. C4:GetDriverConfigInfoById
-- is OS 4.2+; on anything older this returns nil and the port falls back to
-- 9600 8N1 unless the installer picks a baud in Properties.
function IO.ReadBoundSerialSettings(deviceId)
	if (not (C4 and C4.GetDriverConfigInfoById and deviceId)) then
		return nil
	end
	for _, tag in ipairs({'config', 'capabilities'}) do
		local xml = U.SafeCall(function() return C4:GetDriverConfigInfoById(tonumber(deviceId), tag) end)
		if (type(xml) == 'string') then
			local text = string.match(xml, '<serialsettings>(.-)</serialsettings>')
			if (text) then
				return IO.ParseSerialSettings(text)
			end
		end
	end
	return nil
end

local function BoundSerial(binding)
	local b = IO.bound[binding]
	return b and b.serial or nil
end

-- The baud an IR port in serial mode uses when its property is Auto: the
-- bound device driver's, else 9600.
function IO.AutoIrBaud(n)
	local s = BoundSerial(IO.IrBindingId(n, 'serial'))
	local baud = s and s.baud
	if (s and s.format ~= '8N1') then
		U.Log('IR ' .. n .. ': the bound device wants ' .. s.format .. ' but IR serial ports are 8N1 only')
	end
	if (not U.Contains(ZRP.BAUDS, baud)) then
		baud = 9600
	end
	return baud
end

function IO.DesiredIrMode(n)
	local cfg = IO.irConfig[n]
	if (cfg.mode ~= 'serial') then
		return 'ir'
	end
	local baud = tonumber(cfg.baud) or IO.AutoIrBaud(n)
	if (not U.Contains(ZRP.BAUDS, baud)) then
		baud = 9600
	end
	return 'serial ' .. baud .. ' ' .. (cfg.polarity == 'ttl' and 'ttl' or 'rs232')
end

function IO.DesiredSerialConfig(n)
	local cfg = IO.serialConfig[n]
	local s = BoundSerial(ZRP.BIND_SERIAL_BASE + n)
	local baud = tonumber(cfg.baud) or (s and s.baud) or 9600
	if (not U.Contains(ZRP.BAUDS, baud)) then
		baud = 9600
	end
	local format = cfg.format
	if (format == 'auto') then
		format = (s and s.format) or '8N1'
	end
	if (not U.Contains(ZRP.SERIAL_FORMATS, format)) then
		U.Log('RS-232 ' .. n .. ': format ' .. tostring(format) .. ' is not supported by the box; using 8N1')
		format = '8N1'
	end
	return baud .. ' ' .. format
end

local function Norm(s)
	s = string.lower(U.Trim(s))
	return (string.gsub(s, '%s+', ' '))
end

-- The box is the source of truth for port settings. Its retained
-- ir/<n>/mode and serial/<n>/config are ADOPTED: the properties and the
-- IR_OUT / RS_232 connections are changed to match, and nothing is sent
-- back. The driver publishes .../set only when the installer edits a port
-- property in Composer (or a bound device's settings change a port set to
-- Auto), and only if the box does not already have that value. Port
-- settings can therefore be changed in Composer, on the box's web page or
-- over MQTT, and every place ends up showing what the box is doing.

IO.echo = {} -- [property] = value the driver itself just wrote (not a user edit)

-- Writes a property to match the box, remembering it so the resulting
-- OnPropertyChanged (if Director sends one) is not taken for a user edit.
local function SetProp(name, value)
	value = tostring(value)
	if (tostring(U.Prop(name, '')) == value) then
		return
	end
	IO.echo[name] = value
	if (Properties) then
		Properties[name] = value
	end
	U.UpdateProperty(name, value)
end

-- True (once) when an OnPropertyChanged is the echo of SetProp.
function IO.IsEcho(name)
	local v = IO.echo[name]
	if (v == nil) then
		return false
	end
	IO.echo[name] = nil
	return tostring(U.Prop(name, '')) == v
end

local function Push(key, value, boxValue)
	if (boxValue and Norm(boxValue) == Norm(value)) then
		return false
	end
	local p = IO.pending[key]
	if (p and Norm(p.value) == Norm(value) and U.Now() - p.at < 10000) then
		return false
	end
	if (CLIENT.Publish(key .. '/set', value, false)) then
		IO.pending[key] = {value = value, at = U.Now()}
		U.Log('set ' .. key .. ' = ' .. value)
		return true
	end
	return false
end

function IO.PushIr(n)
	return Push('ir/' .. n .. '/mode', IO.DesiredIrMode(n), IO.boxIrMode[n])
end

function IO.PushSerial(n)
	return Push('serial/' .. n .. '/config', IO.DesiredSerialConfig(n), IO.boxSerialConfig[n])
end

-- The Resend Port Configuration action: send every port's properties to the
-- box, whatever it has.
function IO.PushAll()
	if (not CLIENT.IsUp()) then
		return
	end
	for n = 1, ZRP.IR_PORTS do
		CLIENT.Publish('ir/' .. n .. '/mode/set', IO.DesiredIrMode(n), false)
	end
	for n = 1, ZRP.SERIAL_PORTS do
		CLIENT.Publish('serial/' .. n .. '/config/set', IO.DesiredSerialConfig(n), false)
	end
end

local function UpdatePortSummary()
	local parts = {}
	for n = 1, ZRP.IR_PORTS do
		table.insert(parts, 'IR' .. n .. ': ' .. tostring(IO.boxIrMode[n] or '?'))
	end
	for n = 1, ZRP.SERIAL_PORTS do
		table.insert(parts, 'RS232-' .. n .. ': ' .. tostring(IO.boxSerialConfig[n] or '?'))
	end
	U.UpdateProperty('Box Port Settings', table.concat(parts, ', '))
end

local function ShowIrProps(n)
	local serial = (IO.irConfig[n].mode == 'serial')
	U.PropertyVisible('IR ' .. n .. ' Serial Baud', serial)
	U.PropertyVisible('IR ' .. n .. ' Serial Polarity', serial)
end

-- "ir" or "serial <baud> [rs232|ttl]" -> mode, baud, polarity
function IO.ParseIrMode(payload)
	local p = Norm(payload)
	if (p == 'ir') then
		return 'ir'
	end
	local baud, pol = string.match(p, '^serial%s+(%d+)%s*(%a*)')
	if (baud) then
		return 'serial', tonumber(baud), (pol == 'ttl') and 'ttl' or 'rs232'
	end
	return nil
end

function IO.AdoptIr(n, payload)
	IO.boxIrMode[n] = payload
	IO.pending['ir/' .. n .. '/mode'] = nil
	local mode, baud, polarity = IO.ParseIrMode(payload)
	if (not mode) then
		return
	end
	local cfg = IO.irConfig[n]
	local prefix = 'IR ' .. n .. ' '
	cfg.mode = mode
	SetProp(prefix .. 'Mode', mode == 'serial' and 'Serial (one-way)' or 'IR')
	if (mode == 'serial') then
		-- Auto stays Auto when it already gives the box's baud.
		if (not (cfg.baud == 'auto' and IO.AutoIrBaud(n) == baud)) then
			cfg.baud = baud
			SetProp(prefix .. 'Serial Baud', tostring(baud))
		end
		cfg.polarity = polarity
		SetProp(prefix .. 'Serial Polarity', polarity == 'ttl' and 'TTL (idle high)' or 'RS-232 (idle low)')
	end
	ShowIrProps(n)
	if (STATE.initDone) then
		IO.ApplyIrBindings()
	end
end

function IO.AdoptSerial(n, payload)
	IO.boxSerialConfig[n] = payload
	IO.pending['serial/' .. n .. '/config'] = nil
	local baud, format = string.match(U.Trim(payload), '^(%d+)%s+(%w+)')
	baud = tonumber(baud)
	if (not baud) then
		return
	end
	format = string.upper(format)
	local cfg = IO.serialConfig[n]
	local s = BoundSerial(ZRP.BIND_SERIAL_BASE + n)
	if (not (cfg.baud == 'auto' and ((s and s.baud) or 9600) == baud)) then
		cfg.baud = baud
		SetProp('RS-232 ' .. n .. ' Baud', tostring(baud))
	end
	if (not (cfg.format == 'auto' and ((s and s.format) or '8N1') == format)) then
		cfg.format = format
		SetProp('RS-232 ' .. n .. ' Format', format)
	end
end

-- Relays, triggers, contacts ------------------------------------------------------

local function OutputBinding(kind, n)
	return (kind == 'trigger' and ZRP.BIND_TRIGGER_BASE or ZRP.BIND_RELAY_BASE) + n
end

local function Output(kind, n)
	local key = kind .. n
	IO.outputs[key] = IO.outputs[key] or {init = false}
	return IO.outputs[key]
end

function IO.SetOutput(kind, n, on)
	local o = Output(kind, n)
	o.pulse = U.CancelTimer(o.pulse)
	return CLIENT.Publish(kind .. '/' .. n .. '/set', on and 'ON' or 'OFF', false)
end

function IO.PulseOutput(kind, n, ms)
	ms = tonumber(ms)
	if (not ms or ms <= 0) then
		ms = tonumber(U.Prop('Relay Pulse Time (ms)', 500)) or 500
	end
	if (not IO.SetOutput(kind, n, true)) then
		return false
	end
	local o = Output(kind, n)
	o.pulse = U.SetTimer(ms, function()
		o.pulse = nil
		CLIENT.Publish(kind .. '/' .. n .. '/set', 'OFF', false)
	end)
	return true
end

function IO.ToggleOutput(kind, n)
	local o = Output(kind, n)
	return IO.SetOutput(kind, n, not (o.on == true))
end

local function NotifyOutput(kind, n, initial)
	local o = Output(kind, n)
	if (o.on == nil) then
		return
	end
	local binding = OutputBinding(kind, n)
	if (initial) then
		U.SendToProxy(binding, o.on and 'STATE_CLOSED' or 'STATE_OPENED', {})
	else
		U.SendToProxy(binding, o.on and 'CLOSED' or 'OPENED', {})
	end
end

-- ON = energised = contact closed.
function IO.OnOutputState(kind, n, payload)
	local on = (string.upper(U.Trim(payload)) == 'ON')
	local o = Output(kind, n)
	local initial = not o.init
	local changed = (o.on ~= on)
	o.on = on
	o.init = true
	if (initial or changed) then
		NotifyOutput(kind, n, initial)
		UI.PushControl()
	end
end

local function NotifyContact(n, initial)
	local c = IO.contacts[n]
	if (not c or c.closed == nil) then
		return
	end
	local binding = ZRP.BIND_CONTACT_BASE + n
	if (initial) then
		U.SendToProxy(binding, c.closed and 'STATE_CLOSED' or 'STATE_OPENED', {})
	else
		U.SendToProxy(binding, c.closed and 'CLOSED' or 'OPENED', {})
	end
end

function IO.OnContactState(n, payload)
	local closed = (string.upper(U.Trim(payload)) == 'CLOSED')
	IO.contacts[n] = IO.contacts[n] or {init = false}
	local c = IO.contacts[n]
	local initial = not c.init
	local changed = (c.closed ~= closed)
	c.closed = closed
	c.init = true
	if (initial or changed) then
		NotifyContact(n, initial)
		UI.PushControl()
	end
end

-- IR ---------------------------------------------------------------------------------

local function HexWords(text)
	local words = {}
	for w in string.gmatch(text, '%S+') do
		if (not string.match(w, '^%x%x%x%x$')) then
			return nil
		end
		table.insert(words, string.upper(w))
	end
	return words
end

-- Builds the payload for ir/<n>/send from a Control4 <pattern> (Pronto hex)
-- and <repeatcount>.
--
-- The box plays the once-section of a Pronto code, or the repeat-section when
-- there is no once-section, exactly one time. Control4 codes usually carry the
-- whole frame in the repeat-section and ask for it N times, so the frame is
-- unrolled into the once-section N times. The result is ordinary Pronto; if
-- it would not fit in one message the repeat count is reduced.
function IO.BuildPronto(pattern, repeatCount, maxLen)
	local text = U.Trim(string.gsub(tostring(pattern or ''), '%s+', ' '))
	local words = HexWords(text)
	if (not words or #words < 4 or words[1] ~= '0000') then
		return text -- not raw Pronto: pass through untouched
	end
	local once = tonumber(words[3], 16)
	local rpt = tonumber(words[4], 16)
	if (#words < 4 + 2 * (once + rpt) or rpt == 0) then
		return table.concat(words, ' ')
	end
	local onceWords = {}
	for i = 5, 4 + 2 * once do table.insert(onceWords, words[i]) end
	local rptWords = {}
	for i = 5 + 2 * once, 4 + 2 * (once + rpt) do table.insert(rptWords, words[i]) end
	local n = math.floor(tonumber(repeatCount) or 1)
	if (n < 1) then n = 1 end
	if (n > 10) then n = 10 end
	maxLen = maxLen or 700
	local onceText = table.concat(onceWords, ' ')
	local rptText = table.concat(rptWords, ' ')
	while (true) do
		local parts = {'0000', words[2], string.format('%04X', once + n * rpt), '0000'}
		if (onceText ~= '') then table.insert(parts, onceText) end
		for _ = 1, n do table.insert(parts, rptText) end
		local out = table.concat(parts, ' ')
		if (string.len(out) <= maxLen or n == 1) then
			return out
		end
		n = n - 1
	end
end

-- How long the box will be busy sending an IR payload, in ms. Pronto words
-- after the header are carrier periods; the carrier is 1e6 / (f * 0.241246) Hz.
function IO.IrDurationMs(payload)
	local repeats = string.match(payload, '^[nN][eE][cC]%s+%S+%s+%S+%s+(%d+)')
	if (string.match(payload, '^[nN][eE][cC]')) then
		return 110 * (1 + (tonumber(repeats) or 0))
	end
	local words = HexWords(payload)
	if (not words or #words < 5) then
		return 0
	end
	local f = tonumber(words[2], 16)
	local counts = 0
	for i = 5, #words do
		counts = counts + tonumber(words[i], 16)
	end
	return math.floor(counts * f * 0.241246 / 1000)
end

function IO.SendIR(n, code, repeatCount)
	local topic = 'ir/' .. n .. '/send'
	local maxLen = ZRP.MAX_PACKET - 8 - string.len(CLIENT.Topic(topic))
	local payload
	if (string.match(U.Trim(code), '^[nN][eE][cC]%s')) then
		payload = U.Trim(code)
	else
		payload = IO.BuildPronto(code, repeatCount, maxLen)
	end
	if (payload == '') then
		U.Log('IR ' .. n .. ': empty code ignored')
		return false
	end
	return CLIENT.Publish(topic, payload, false, IO.IrDurationMs(payload))
end

local function ProxyData(params)
	params = params or {}
	return params.data or params.DATA or params.Data
end

local function HandleIrFromProxy(n, command, params)
	local cmd = string.upper(tostring(command or ''))
	if (cmd == 'STOP' or string.match(cmd, 'STOP')) then
		return true -- nothing to stop: the box sends a whole code at once
	end
	local data = ProxyData(params)
	if (type(data) ~= 'string') then
		return false
	end
	local pattern = string.match(data, '<pattern>(.-)</pattern>')
	local repeatCount = tonumber(string.match(data, '<repeatcount>(.-)</repeatcount>') or '')
	if (not pattern and string.match(U.Trim(data), '^%x%x%x%x%s')) then
		pattern = data
	end
	if (not pattern) then
		return false
	end
	if (IO.irConfig[n].mode ~= 'ir') then
		U.Log('IR ' .. n .. ' is in serial mode; IR code ignored')
		return true
	end
	IO.SendIR(n, pattern, repeatCount)
	return true
end

-- Serial -------------------------------------------------------------------------------

-- kind: 'serial' (RS-232 1-2) or 'irserial' (IR port in one-way serial mode).
-- Long strings go in 256-byte pieces: the box sends each payload as it is,
-- one after another, so splitting changes nothing on the wire.
function IO.SendSerial(kind, n, data)
	data = tostring(data or '')
	if (data == '') then
		return false
	end
	local topic, baud
	if (kind == 'serial') then
		topic = 'serial/' .. n .. '/tx'
		baud = tonumber(string.match(IO.DesiredSerialConfig(n), '^(%d+)'))
	else
		if (IO.irConfig[n].mode ~= 'serial') then
			U.Log('IR ' .. n .. ' is not in serial mode; serial data ignored')
			return false
		end
		topic = 'ir/' .. n .. '/tx'
		baud = tonumber(string.match(IO.DesiredIrMode(n), '^serial (%d+)'))
	end
	baud = baud or 9600
	local ok = true
	local pos = 1
	while (pos <= string.len(data)) do
		local chunk = string.sub(data, pos, pos + 255)
		-- 10 bits a byte on the wire while the box is busy sending it.
		local hold = math.floor(string.len(chunk) * 10 * 1000 / baud)
		ok = CLIENT.Publish(topic, chunk, false, hold) and ok
		pos = pos + 256
	end
	return ok
end

-- What Director sends a DriverWorks RS_232 *provider* is not documented (see
-- README). Accept the data under any of the likely parameter names, pick up
-- serial settings if they are offered, and log anything else once so it can
-- be identified on a real system.
local function HandleSerialFromProxy(binding, info, command, params)
	params = params or {}
	local data = ProxyData(params) or params.STRING or params.PAYLOAD or params.COMMAND_STRING
	if (type(data) == 'string' and data ~= '') then
		IO.SendSerial(info.kind, info.port, data)
		return true
	end
	local baud = tonumber(params.BAUD_RATE or params.BAUD or params.baud or params.baud_rate)
	local settings = params.SERIAL_SETTINGS or params.serialsettings or params.SETTINGS
	if (baud or type(settings) == 'string') then
		local s = (type(settings) == 'string') and IO.ParseSerialSettings(settings) or {baud = baud, format = '8N1'}
		if (s) then
			IO.bound[binding] = IO.bound[binding] or {}
			IO.bound[binding].serial = s
			U.Log('serial settings for binding ' .. binding .. ': ' .. s.baud .. ' ' .. s.format)
			IO.BoundSettingsChanged(info)
		end
		return true
	end
	return false
end

function IO.OnSerialRx(n, payload)
	local text = string.gsub(tostring(payload or ''), '[\r\n]+$', '')
	U.SetVariable('SERIAL_' .. n .. '_RX', text)
	IO.serialRx[n] = {text = U.Printable(payload, 300), at = U.Now()}
	U.FireEvent('RS-232 ' .. n .. ' Data Received')
	UI.PushControl()
end

-- Director -> driver ---------------------------------------------------------------

local function LogUnknown(binding, command, params)
	local key = tostring(binding) .. ':' .. tostring(command)
	local parts = {}
	for k, v in pairs(params or {}) do
		table.insert(parts, tostring(k) .. '=' .. U.Printable(v, 200))
	end
	local text = 'unhandled ' .. tostring(command) .. ' on binding ' .. tostring(binding) .. ' {' .. table.concat(parts, ', ') .. '}'
	if (not IO.unknownLogged[key]) then
		IO.unknownLogged[key] = true
		U.Log(text)
		U.UpdateProperty('Last Unhandled Proxy Command', text)
	else
		U.Debug(text)
	end
end

-- ReceivedFromProxy for our control bindings. Returns true if the binding is ours.
function IO.FromProxy(binding, command, params)
	local info = IO.BindingInfo(binding)
	if (not info) then
		return false
	end
	local cmd = string.upper(tostring(command or ''))
	local handled = false
	if (info.kind == 'ir') then
		handled = HandleIrFromProxy(info.port, cmd, params)
	elseif (info.kind == 'irserial' or info.kind == 'serial') then
		handled = HandleSerialFromProxy(binding, info, cmd, params)
	elseif (info.kind == 'relay' or info.kind == 'trigger') then
		handled = true
		if (cmd == 'OPEN') then
			IO.SetOutput(info.kind, info.port, false)
		elseif (cmd == 'CLOSE') then
			IO.SetOutput(info.kind, info.port, true)
		elseif (cmd == 'TOGGLE') then
			IO.ToggleOutput(info.kind, info.port)
		elseif (cmd == 'TRIGGER') then
			IO.PulseOutput(info.kind, info.port, params and params.TIME)
		elseif (cmd == 'GET_STATE') then
			NotifyOutput(info.kind, info.port, true)
		else
			handled = false
		end
	elseif (info.kind == 'contact') then
		if (cmd == 'GET_STATE') then
			NotifyContact(info.port, true)
			handled = true
		end
	end
	if (not handled) then
		LogUnknown(binding, command, params)
	end
	return true
end

function IO.OnBindingChanged(binding, class, bound, otherDevice, otherBinding)
	local info = IO.BindingInfo(binding)
	if (not info) then
		return false
	end
	if (not bound) then
		IO.bound[binding] = nil
		return true
	end
	IO.bound[binding] = {device = tonumber(otherDevice), otherBinding = tonumber(otherBinding)}
	if (info.kind == 'relay' or info.kind == 'trigger') then
		NotifyOutput(info.kind, info.port, true)
	elseif (info.kind == 'contact') then
		NotifyContact(info.port, true)
	elseif (info.kind == 'serial' or info.kind == 'irserial') then
		local s = IO.ReadBoundSerialSettings(otherDevice)
		if (s) then
			IO.bound[binding].serial = s
			U.Log('binding ' .. binding .. ' bound to a device that wants ' .. s.baud .. ' ' .. s.format)
			IO.BoundSettingsChanged(info)
		end
	end
	return true
end

-- Box -> driver ----------------------------------------------------------------------

function IO.OnMessage(sub, payload, retain)
	local parts = U.Split(sub, '/')
	local head, n, leaf = parts[1], tonumber(parts[2]), parts[3]
	if (parts[#parts] == 'set') then
		return -- a command (our own, echoed by a stock broker), not state
	end
	if (head == 'relay' and n and leaf == 'state' and n <= ZRP.RELAYS) then
		IO.OnOutputState('relay', n, payload)
	elseif (head == 'trigger' and n and leaf == 'state' and n <= ZRP.TRIGGERS) then
		IO.OnOutputState('trigger', n, payload)
	elseif (head == 'sense' and n and leaf == 'state' and n <= ZRP.CONTACTS) then
		IO.OnContactState(n, payload)
	elseif (head == 'ir' and n and leaf == 'mode' and n <= ZRP.IR_PORTS) then
		IO.AdoptIr(n, payload)
		UpdatePortSummary()
		UI.PushControl()
	elseif (head == 'serial' and n and leaf == 'config' and n <= ZRP.SERIAL_PORTS) then
		IO.AdoptSerial(n, payload)
		UpdatePortSummary()
		UI.PushControl()
	elseif (head == 'serial' and n and leaf == 'rx' and n <= ZRP.SERIAL_PORTS) then
		IO.OnSerialRx(n, payload)
	elseif (head == 'ir' and n and leaf == 'sent') then
		U.Debug('IR ' .. n .. ' sent')
	elseif (head == 'remote' and parts[2] == 'code') then
		REMOTES.OnCode(payload)
	elseif (sub == 'status') then
		IO.boxStatus = payload
		U.UpdateProperty('Box Status', payload)
		UI.PushControl()
	elseif (sub == 'firmware/version') then
		IO.firmware = payload
		U.UpdateProperty('Box Firmware', payload)
		UI.PushControl()
	end
end

function IO.OnUp()
	U.SetVariable('CONNECTED', '1')
	U.FireEvent('Connected')
	UI.PushState()
	UI.PushControl()
end

function IO.OnDown(reason)
	U.SetVariable('CONNECTED', '0')
	U.UpdateProperty('Box Status', 'unreachable')
	U.FireEvent('Disconnected')
	IO.pending = {}
	IO.boxStatus = 'unreachable'
	UI.PushState()
	UI.PushControl()
end

-- Properties -> config ------------------------------------------------------------------

function IO.ParseBaud(value)
	return tonumber(string.match(tostring(value or ''), '^(%d+)')) or 'auto'
end

function IO.ReadIrProperties(n)
	local mode = string.lower(tostring(U.Prop('IR ' .. n .. ' Mode', 'IR')))
	local cfg = IO.irConfig[n]
	cfg.mode = (string.find(mode, 'serial', 1, true) and 'serial') or 'ir'
	cfg.baud = IO.ParseBaud(U.Prop('IR ' .. n .. ' Serial Baud', 'Auto'))
	cfg.polarity = string.find(string.lower(tostring(U.Prop('IR ' .. n .. ' Serial Polarity', 'RS-232'))), 'ttl', 1, true) and 'ttl' or 'rs232'
	ShowIrProps(n)
end

-- A bound device's serial settings became known or changed: push them only
-- to a port whose baud/format property is Auto.
function IO.BoundSettingsChanged(info)
	if (info.kind == 'irserial') then
		local cfg = IO.irConfig[info.port]
		if (cfg.mode == 'serial' and cfg.baud == 'auto') then
			IO.PushIr(info.port)
		end
	elseif (info.kind == 'serial') then
		local cfg = IO.serialConfig[info.port]
		if (cfg.baud == 'auto' or cfg.format == 'auto') then
			IO.PushSerial(info.port)
		end
	end
end

function IO.ReadSerialProperties(n)
	local cfg = IO.serialConfig[n]
	cfg.baud = IO.ParseBaud(U.Prop('RS-232 ' .. n .. ' Baud', 'Auto'))
	local format = U.Trim(U.Prop('RS-232 ' .. n .. ' Format', 'Auto'))
	cfg.format = U.Contains(ZRP.SERIAL_FORMATS, format) and format or 'auto'
end
