-- The "Remote Buttons" tab in Composer: www/html/index.html, declared in
-- driver.xml as <tabs><tab name="Remote Buttons" file="html/index.html"/>.
--
-- Composer opens the page with ?apihost=&driverid=&apitoken= and the page
-- talks to this driver through Director's REST API, the same bridge
-- finitelabs' shipping tab drivers use (control4-finite-labs-essentials
-- drivers/network_requests, control4-influxdb drivers/influxdb):
--
--   page -> driver  POST /api/v1/items/<driverid>/commands
--                   {command, tParams = {DATA = base64(JSON)}, async = false}
--                   arrives here as UIRequest(command, tParams)
--   driver -> page  C4:SendDataToUI(command, {json = <JSON>}), delivered to
--                   the page's socket.io subscription (/api/v1/items/<id>/datatoui)
--
-- Every UI_* request answers with a push, and returns the same JSON for a
-- caller that reads the REST reply. Pushes also go out on their own when a
-- remote button arrives, learning finishes, or the box connects/drops, so
-- the page stays live without polling.

UI.handlers = {}

local function Encode(value)
	return JSON.Encode(value)
end

-- tParams.DATA is base64 JSON from the page. Plain tParams (tests, other
-- callers) are accepted as they are.
function UI.Params(tParams)
	tParams = tParams or {}
	local data = tParams.DATA
	if (type(data) == 'string' and data ~= '' and C4 and C4.Base64Decode) then
		local raw = U.SafeCall(function() return C4:Base64Decode(data) end)
		local decoded = raw and JSON.Decode(raw)
		if (type(decoded) == 'table') then
			return decoded
		end
	end
	return tParams
end

-- Sends one message to the page. SendDataToUI cannot carry booleans (see
-- finitelabs' uiRespond), so everything travels as a single JSON string.
-- Returns {"_command": command, "data": value} as JSON: UIRequest's answer
-- for a caller that reads the REST reply.
function UI.Send(command, value)
	local json = Encode(value)
	if (STATE.initDone and C4 and C4.SendDataToUI) then
		U.SafeCall(function() C4:SendDataToUI(command, {json = json}) end)
	end
	return '{"_command":' .. Encode(command) .. ',"data":' .. json .. '}'
end

local function ButtonView(btn)
	local actions = JSON.Array({})
	local summary = {}
	for _, a in ipairs(btn.actions or {}) do
		table.insert(actions, a)
		table.insert(summary, ROOMS.Describe(a))
	end
	return {
		code = btn.code,
		remote = btn.remote,
		remoteName = REMOTES.RemoteName(btn.remote),
		name = btn.name,
		event = btn.event,
		eventName = REMOTES.EventName(btn),
		actions = actions,
		summary = table.concat(summary, '; '),
	}
end

function UI.State()
	local remotes = JSON.Array({})
	local ids = {}
	for id, _ in pairs(REMOTES.data.remotes) do table.insert(ids, id) end
	table.sort(ids, function(a, b) return string.lower(REMOTES.RemoteName(a)) < string.lower(REMOTES.RemoteName(b)) end)
	for _, id in ipairs(ids) do
		table.insert(remotes, {id = id, name = REMOTES.RemoteName(id), buttons = #REMOTES.ButtonsOf(id)})
	end
	local buttons = JSON.Array({})
	for _, btn in ipairs(REMOTES.SortedButtons()) do
		table.insert(buttons, ButtonView(btn))
	end
	local recent = JSON.Array({})
	local now = U.Now()
	for _, r in ipairs(REMOTES.recent) do
		local btn = REMOTES.data.buttons[r.code]
		table.insert(recent, {code = r.code, ago = math.floor((now - r.at) / 1000),
			known = btn ~= nil, label = btn and REMOTES.Label(btn) or nil, remote = (REMOTES.Parse(r.code))})
	end
	local learn = REMOTES.learn
	return {
		version = DRIVER_VERSION,
		connected = CLIENT.IsUp(),
		status = CLIENT.statusText or '',
		learn = {
			armed = learn.armed,
			name = learn.name,
			remaining = learn.armed and math.max(0, math.floor((learn.expires - now) / 1000)) or 0,
			captured = learn.captured,
		},
		remotes = remotes,
		buttons = buttons,
		recent = recent,
	}
end

-- Pushes the whole state; the page redraws from it.
function UI.PushState()
	return UI.Send('STATE_DATA', UI.State())
end

-- Requests ------------------------------------------------------------------------

function UI.handlers.UI_GET_STATE()
	return UI.PushState()
end

-- Rooms and sources normally come straight from Director's REST API in the
-- page; these two are its fallback when that is not reachable.
function UI.handlers.UI_GET_ROOMS(p)
	local rooms = JSON.Array({})
	for _, r in ipairs(ROOMS.List(U.IsTrue(p.refresh))) do
		table.insert(rooms, r)
	end
	return UI.Send('ROOMS_DATA', {rooms = rooms})
end

function UI.handlers.UI_GET_SOURCES(p)
	local list = JSON.Array({})
	for _, s in ipairs(ROOMS.Sources(p.room)) do
		table.insert(list, s)
	end
	return UI.Send('SOURCES_DATA', {room = tonumber(p.room), sources = list})
end

function UI.handlers.UI_LEARN(p)
	REMOTES.Arm(p.name, p.seconds)
	return UI.PushState()
end

function UI.handlers.UI_CANCEL_LEARN()
	REMOTES.Disarm('Cancelled')
	return UI.PushState()
end

function UI.handlers.UI_SAVE_BUTTON(p)
	local btn, err = REMOTES.Upsert(p)
	if (not btn) then
		return UI.Send('ERROR', {message = err})
	end
	UI.Send('SAVED', {code = btn.code, remote = btn.remote})
	return UI.PushState()
end

function UI.handlers.UI_DELETE_BUTTON(p)
	REMOTES.Delete(U.Trim(p.code))
	return UI.PushState()
end

function UI.handlers.UI_TEST_BUTTON(p)
	local btn = REMOTES.data.buttons[U.Trim(p.code)]
	if (not btn) then
		return UI.Send('ERROR', {message = 'unknown button'})
	end
	U.FireEvent(REMOTES.EventName(btn))
	local ok = REMOTES.Execute(btn)
	if (not ok) then
		return UI.Send('ERROR', {message = STATE.lastError or 'test failed'})
	end
	return UI.Send('TESTED', {code = btn.code})
end

function UI.handlers.UI_RENAME_REMOTE(p)
	REMOTES.RenameRemote(tostring(p.id or ''), p.name)
	return UI.PushState()
end

-- The Control tab (www/html/control.html) ------------------------------------------

local function OutputView(kind, count)
	local list = JSON.Array({})
	for n = 1, count do
		local o = IO.outputs[kind .. n]
		local state = '?'
		if (o and o.on ~= nil) then state = o.on and 'ON' or 'OFF' end
		table.insert(list, {n = n, state = state})
	end
	return list
end

-- Everything the Control tab shows. Pushed as CONTROL_DATA on every change.
function UI.ControlState()
	local contacts, ir, serial = JSON.Array({}), JSON.Array({}), JSON.Array({})
	for n = 1, ZRP.CONTACTS do
		local c = IO.contacts[n]
		local state = '?'
		if (c and c.closed ~= nil) then state = c.closed and 'CLOSED' or 'OPEN' end
		table.insert(contacts, {n = n, state = state})
	end
	for n = 1, ZRP.IR_PORTS do
		table.insert(ir, {n = n, mode = IO.boxIrMode[n] or IO.DesiredIrMode(n), serial = IO.irConfig[n].mode == 'serial',
			highOut = (n == 7)})
	end
	local now = U.Now()
	for n = 1, ZRP.SERIAL_PORTS do
		local rx = IO.serialRx[n]
		table.insert(serial, {n = n, config = IO.boxSerialConfig[n] or IO.DesiredSerialConfig(n),
			rx = rx and rx.text or '', rxAgo = rx and math.floor((now - rx.at) / 1000) or -1})
	end
	local cfg = CLIENT.cfg
	return {
		version = DRIVER_VERSION,
		connected = CLIENT.IsUp(),
		status = CLIENT.statusText or '',
		boxStatus = IO.boxStatus,
		address = (cfg.ip ~= '' and (cfg.ip .. ':' .. cfg.port) or ''),
		mode = PROPS.IsBroker() and 'Central broker' or 'Direct to box (P2P)',
		firmware = IO.firmware,
		pulseMs = tonumber(U.Prop('Relay Pulse Time (ms)', 500)) or 500,
		relays = OutputView('relay', ZRP.RELAYS),
		triggers = OutputView('trigger', ZRP.TRIGGERS),
		contacts = contacts,
		ir = ir,
		serial = serial,
	}
end

function UI.PushControl()
	return UI.Send('CONTROL_DATA', UI.ControlState())
end

local function NotConnected()
	if (CLIENT.IsUp()) then
		return nil
	end
	return UI.Send('ERROR', {message = 'Not connected to the box'})
end

function UI.handlers.UI_CONTROL_GET_STATE()
	return UI.PushControl()
end

function UI.handlers.UI_SET_OUTPUT(p)
	local kind, n, action = tostring(p.kind or ''), tonumber(p.n), string.upper(tostring(p.action or ''))
	local count = (kind == 'relay' and ZRP.RELAYS) or (kind == 'trigger' and ZRP.TRIGGERS) or 0
	if (not n or n < 1 or n > count) then
		return UI.Send('ERROR', {message = 'no such output'})
	end
	local off = NotConnected()
	if (off) then return off end
	if (action == 'ON') then
		IO.SetOutput(kind, n, true)
	elseif (action == 'OFF') then
		IO.SetOutput(kind, n, false)
	elseif (action == 'PULSE') then
		IO.PulseOutput(kind, n, nil)
	else
		return UI.Send('ERROR', {message = 'unknown action ' .. action})
	end
	return UI.Send('SENT', {what = kind .. ' ' .. n .. ' ' .. string.lower(action)})
end

function UI.handlers.UI_SEND_IR(p)
	local n, code = tonumber(p.port), U.Trim(p.code)
	if (not n or n < 1 or n > ZRP.IR_PORTS or code == '') then
		return UI.Send('ERROR', {message = 'need an IR port and a code'})
	end
	if (IO.irConfig[n].mode ~= 'ir') then
		return UI.Send('ERROR', {message = 'IR ' .. n .. ' is in serial mode'})
	end
	local off = NotConnected()
	if (off) then return off end
	if (not IO.SendIR(n, code, 1)) then
		return UI.Send('ERROR', {message = STATE.lastError or 'not sent'})
	end
	return UI.Send('SENT', {what = 'IR ' .. n})
end

-- p: {kind = 'serial' | 'ir', port, data, escapes = bool}
function UI.handlers.UI_SEND_SERIAL(p)
	local n = tonumber(p.port)
	local kind = (p.kind == 'ir') and 'irserial' or 'serial'
	local max = (kind == 'irserial') and ZRP.IR_PORTS or ZRP.SERIAL_PORTS
	local data = tostring(p.data or '')
	if (p.escapes == true or p.escapes == 'true') then
		data = U.DecodeEscapes(data)
	end
	if (not n or n < 1 or n > max or data == '') then
		return UI.Send('ERROR', {message = 'need a port and some data'})
	end
	if (kind == 'irserial' and IO.irConfig[n].mode ~= 'serial') then
		return UI.Send('ERROR', {message = 'IR ' .. n .. ' is not in serial mode'})
	end
	local off = NotConnected()
	if (off) then return off end
	IO.SendSerial(kind, n, data)
	return UI.Send('SENT', {what = ((kind == 'irserial') and 'IR ' or 'RS-232 ') .. n .. ': ' .. string.len(data) .. ' bytes'})
end

-- Returns the JSON answer, or nil for a command that is not ours.
function UI.Request(command, tParams)
	local handler = UI.handlers[string.upper(tostring(command or ''))]
	if (not handler) then
		return nil
	end
	local ok, result = pcall(handler, UI.Params(tParams))
	if (not ok) then
		U.Log('UI request ' .. tostring(command) .. ' failed: ' .. tostring(result))
		return UI.Send('ERROR', {message = tostring(result)})
	end
	return result
end
