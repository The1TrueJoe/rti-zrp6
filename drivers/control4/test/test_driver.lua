-- Unit tests for the ZRP-6 Control4 driver, against the mock in mock_c4.lua.
--
--   lua test/test_driver.lua build/stage/driver.lua
--   luajit test/test_driver.lua build/stage/driver.lua
--
-- The MQTT bytes the driver sends are decoded here by an independent
-- decoder, and everything the "box" sends is hand-built, so the driver's own
-- codec is never used to check itself.

local here = (arg and arg[0] or ''):match('^(.*)[/\\]') or '.'
package.path = here .. '/?.lua;' .. package.path
local Mock = require('mock_c4')
local DRIVER = arg and arg[1] or 'build/stage/driver.lua'

-- tiny test framework ----------------------------------------------------------

local passed, failed, current = 0, 0, ''
local function test(name, fn)
	current = name
	local ok, err = xpcall(fn, debug.traceback)
	if (ok) then
		passed = passed + 1
		io.write('  ok    ', name, '\n')
	else
		failed = failed + 1
		io.write('  FAIL  ', name, '\n        ', tostring(err):gsub('\n', '\n        '), '\n')
	end
end
local function show(v)
	if (type(v) == 'string') then return string.format('%q', v) end
	return tostring(v)
end
local function eq(a, b, msg)
	if (a ~= b) then
		error((msg or 'values differ') .. ': expected ' .. show(b) .. ', got ' .. show(a), 2)
	end
end
local function truthy(v, msg)
	if (not v) then error(msg or 'expected a true value', 2) end
end

-- independent MQTT helpers -----------------------------------------------------------

local function enc_len(n)
	local out = {}
	repeat
		local d = n % 128
		n = math.floor(n / 128)
		if (n > 0) then d = d + 128 end
		out[#out + 1] = string.char(d)
	until n == 0
	return table.concat(out)
end
local function u16(n) return string.char(math.floor(n / 256), n % 256) end
local function mstr(s) return u16(#s) .. s end
local function box_publish(topic, payload, retain)
	local body = mstr(topic) .. payload
	return string.char(0x30 + (retain and 1 or 0)) .. enc_len(#body) .. body
end
local function connack(rc) return string.char(0x20, 2, 0, rc) end
local function suback(id) return string.char(0x90, 3) .. u16(id) .. string.char(0) end
local PINGRESP = string.char(0xD0, 0)

-- Splits a byte string into packets: {type, flags, body}.
local function split_packets(data)
	local out, pos = {}, 1
	while (pos <= #data) do
		local first = data:byte(pos)
		local mult, len, i = 1, 0, pos + 1
		repeat
			local b = data:byte(i)
			len = len + (b % 128) * mult
			mult = mult * 128
			i = i + 1
		until b < 128
		out[#out + 1] = {type = math.floor(first / 16), flags = first % 16, body = data:sub(i, i + len - 1)}
		pos = i + len
	end
	return out
end

local function sent_packets(C4, from)
	local out = {}
	for idx = (from or 1), #C4.net.sent do
		for _, p in ipairs(split_packets(C4.net.sent[idx].data)) do
			p.at = C4.net.sent[idx].at
			out[#out + 1] = p
		end
	end
	return out
end

local function publishes(C4, from)
	local out = {}
	for _, p in ipairs(sent_packets(C4, from)) do
		if (p.type == 3) then
			local tl = p.body:byte(1) * 256 + p.body:byte(2)
			out[#out + 1] = {topic = p.body:sub(3, 2 + tl), payload = p.body:sub(3 + tl), retain = p.flags % 2 == 1, at = p.at}
		end
	end
	return out
end

local function find_pub(C4, topic, from)
	for _, p in ipairs(publishes(C4, from)) do
		if (p.topic == topic) then return p end
	end
	return nil
end

-- driver instances ---------------------------------------------------------------------

local DEFAULT_PROPS = {
	['Box IP Address'] = '127.0.0.1', ['MQTT Port'] = '1883', ['Username'] = 'admin', ['Password'] = 'admin',
	['Base Topic'] = 'zrp6', ['Keepalive (s)'] = '30', ['Debug Mode'] = 'Off', ['Relay Pulse Time (ms)'] = '500',
}
for n = 1, 7 do
	DEFAULT_PROPS['IR ' .. n .. ' Mode'] = 'IR'
	DEFAULT_PROPS['IR ' .. n .. ' Serial Baud'] = 'Auto (bound device)'
	DEFAULT_PROPS['IR ' .. n .. ' Serial Polarity'] = 'RS-232 (idle low)'
end
for n = 1, 2 do
	DEFAULT_PROPS['RS-232 ' .. n .. ' Baud'] = 'Auto (bound device)'
	DEFAULT_PROPS['RS-232 ' .. n .. ' Format'] = 'Auto (bound device)'
end

local function boot(opts)
	opts = opts or {}
	local C4 = Mock.new(opts)
	local props = {}
	for k, v in pairs(DEFAULT_PROPS) do props[k] = v end
	for k, v in pairs(opts.props or {}) do props[k] = v end
	local env, m = Mock.load(DRIVER, C4, props)
	env.OnDriverInit(0)
	env.OnDriverLateInit(0)
	return {C4 = C4, env = env, m = m, props = props}
end

local function setprop(d, name, value)
	d.props[name] = value
	d.env.OnPropertyChanged(name)
end

local function box(d, data)
	d.env.ReceivedFromNetwork(6001, 1883, data)
end

-- Drives a booted driver to the logged-in, subscribed state.
local function connect(d)
	d.C4:Advance(600)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	box(d, connack(0))
	d.C4:Advance(100)
	box(d, suback(1))
end

local function proxy_calls(C4, binding)
	local out = {}
	for _, c in ipairs(C4.proxy) do
		if (c.binding == binding) then out[#out + 1] = c.command end
	end
	return table.concat(out, ',')
end

-- A request from the Remote Buttons tab: base64 JSON in tParams.DATA, as
-- Director delivers POST /api/v1/items/<id>/commands to UIRequest.
local function ui(d, command, json)
	local reply = d.env.UIRequest(command, {DATA = Mock.b64encode(json or '{}')})
	return reply and d.m.JSON.Decode(reply)
end

-- The last SendDataToUI push of a command, decoded.
local function pushed(d, command)
	for i = #d.C4.dataToUI, 1, -1 do
		local p = d.C4.dataToUI[i]
		if (p.command == command) then return d.m.JSON.Decode(p.params.json) end
	end
	return nil
end

io.write('ZRP-6 driver tests (', _VERSION, jit and (' / ' .. jit.version) or '', ')\n')

-- MQTT codec -----------------------------------------------------------------------------

local probe = boot({props = {['Box IP Address'] = ''}})
local MQTT = probe.m.MQTT

test('remaining length encodes at the 1/2/3/4-byte boundaries', function()
	local cases = {[0] = '00', [127] = '7F', [128] = '8001', [16383] = 'FF7F', [16384] = '808001',
		[2097151] = 'FFFF7F', [2097152] = '80808001', [268435455] = 'FFFFFF7F'}
	for n, hex in pairs(cases) do
		local got = MQTT.EncodeLength(n):gsub('.', function(c) return string.format('%02X', c:byte()) end)
		eq(got, hex, 'EncodeLength(' .. n .. ')')
		local v, used = MQTT.DecodeLength(MQTT.EncodeLength(n), 1)
		eq(v, n, 'DecodeLength round trip'); eq(used, #hex / 2, 'bytes used')
	end
	local v, err = MQTT.DecodeLength(string.char(0x80, 0x80), 1)
	eq(v, nil); eq(err, 'incomplete')
	v, err = MQTT.DecodeLength(string.char(0x80, 0x80, 0x80, 0x80, 0x01), 1)
	eq(v, nil); eq(err, 'malformed')
end)

test('CONNECT carries protocol level 4, clean session, username, password, keepalive', function()
	local pkt = MQTT.Connect({clientId = 'c', username = 'admin', password = 'pw', keepalive = 30})
	local expect = string.char(0x10) .. enc_len(10 + 3 + 7 + 4) .. mstr('MQTT') .. string.char(4, 0xC2) .. u16(30)
		.. mstr('c') .. mstr('admin') .. mstr('pw')
	eq(pkt, expect)
end)

test('SUBSCRIBE / PUBLISH / PINGREQ / DISCONNECT bytes', function()
	eq(MQTT.Subscribe(1, {'zrp6/#'}), string.char(0x82) .. enc_len(2 + 8 + 1) .. u16(1) .. mstr('zrp6/#') .. string.char(0))
	eq(MQTT.Publish('a/b', 'ON', false), string.char(0x30, 7) .. mstr('a/b') .. 'ON')
	eq(MQTT.Publish('a/b', 'ON', true), string.char(0x31, 7) .. mstr('a/b') .. 'ON')
	eq(MQTT.PingReq(), string.char(0xC0, 0))
	eq(MQTT.Disconnect(), string.char(0xE0, 0))
	local big = MQTT.Publish('t', string.rep('x', 200), false)
	eq(big:sub(1, 3), string.char(0x30, 0xCB, 0x01), '2-byte remaining length for 203 bytes')
end)

test('parser: one packet, byte-at-a-time, coalesced, split inside the length', function()
	local p = MQTT.NewParser()
	local one = box_publish('zrp6/relay/1/state', 'ON', true)
	local got = p:Feed(one)
	eq(#got, 1); eq(got[1].type, 3); eq(got[1].flags, 1)

	local long = box_publish('zrp6/ir/1/sent', string.rep('A', 300), false) -- 2-byte length
	local stream = connack(0) .. one .. long .. PINGRESP
	local all = {}
	for i = 1, #stream do
		for _, pkt in ipairs(assert(p:Feed(stream:sub(i, i)))) do all[#all + 1] = pkt end
	end
	eq(#all, 4, 'byte-at-a-time'); eq(all[1].type, 2); eq(all[3].type, 3); eq(#all[3].body, 2 + 14 + 300); eq(all[4].type, 13)

	local batch = assert(p:Feed(stream .. stream))
	eq(#batch, 8, 'coalesced')

	-- split right after the first byte of a 2-byte remaining length
	local a = assert(p:Feed(long:sub(1, 2)))
	eq(#a, 0)
	local b = assert(p:Feed(long:sub(3)))
	eq(#b, 1)
	eq(p.buf, '', 'nothing left over')
end)

test('parser: malformed length and oversized packets are errors', function()
	local p = MQTT.NewParser()
	local r, err = p:Feed(string.char(0x30, 0xFF, 0xFF, 0xFF, 0xFF, 0x01))
	eq(r, nil); truthy(err:find('malformed'))
	r, err = p:Feed(string.char(0x30) .. enc_len(1000000))
	eq(r, nil); truthy(err:find('too large'))
end)

test('PUBLISH decode: QoS 0 and QoS 1 with packet id, binary payload', function()
	local msg = MQTT.DecodePublish(0, mstr('zrp6/serial/1/rx') .. 'A\0B\r\n')
	eq(msg.topic, 'zrp6/serial/1/rx'); eq(msg.payload, 'A\0B\r\n'); eq(msg.qos, 0)
	msg = MQTT.DecodePublish(3, mstr('t') .. u16(77) .. 'x')
	eq(msg.qos, 1); eq(msg.packetId, 77); eq(msg.payload, 'x'); eq(msg.retain, true)
end)

test('JSON round trip', function()
	local JSON = probe.m.JSON
	local v = JSON.Decode('{"a":[1,2,{"b":"x\\n\\u0041"}],"c":true,"d":null,"e":-1.5}')
	eq(v.a[3].b, 'x\nA'); eq(v.c, true); eq(v.d, nil); eq(v.e, -1.5)
	eq(JSON.Encode({code = 'p1:2', list = JSON.Array({}), n = 3}), '{"code":"p1:2","list":[],"n":3}')
	eq(JSON.Decode('{bad'), nil)
end)

-- Connection -------------------------------------------------------------------------------

test('connects once, logs in with the configured user, subscribes to base/#', function()
	local d = boot()
	eq(#d.C4.net.connects, 0, 'no connect before the start delay')
	d.C4:Advance(600)
	eq(#d.C4.net.connects, 1)
	eq(d.C4.net.connects[1].port, 1883)
	eq(d.C4.net.created[#d.C4.net.created].address, '127.0.0.1')
	eq(d.C4.net.options[1].options.KEEP_CONNECTION, false, 'Director must not reconnect on its own')
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	local pk = sent_packets(d.C4)
	eq(#pk, 1); eq(pk[1].type, 1)
	truthy(pk[1].body:find(mstr('admin') .. mstr('admin'), 1, true), 'username and password in CONNECT')
	box(d, connack(0))
	d.C4:Advance(100)
	pk = sent_packets(d.C4)
	eq(pk[2].type, 8, 'SUBSCRIBE after CONNACK')
	truthy(pk[2].body:find(mstr('zrp6/#'), 1, true))
	eq(d.C4.vars.CONNECTED, '1')
	truthy(d.C4.props['Connection Status']:find('^Connected'))
	eq(#d.C4.net.connects, 1, 'still exactly one connection')
end)

test('keepalive: PINGREQ every 15 s; silence for 45 s drops and reconnects', function()
	local d = boot()
	connect(d)
	local before = #sent_packets(d.C4)
	d.C4:Advance(14000)
	eq(#sent_packets(d.C4), before, 'nothing within keepalive/2 of the last send')
	d.C4:Advance(1500)
	local pk = sent_packets(d.C4)
	eq(#pk, before + 1)
	eq(pk[#pk].type, 12, 'PINGREQ after keepalive/2 of quiet')
	box(d, PINGRESP)
	d.C4:Advance(20000)
	eq(#d.C4.net.disconnects, 0, 'answered pings keep it up')
	local silent = 0
	while (#d.C4.net.disconnects == 0 and silent < 60000) do -- no more PINGRESP
		d.C4:Advance(1000); silent = silent + 1000
	end
	silent = silent + 20000 -- the last PINGRESP was 20 s before the loop
	truthy(silent >= 45000 and silent <= 47000, 'dropped after 1.5x keepalive of silence (' .. silent .. ' ms)')
	eq(d.C4.vars.CONNECTED, '0')
	local connects = #d.C4.net.connects
	d.C4:Advance(2100)
	eq(#d.C4.net.connects, connects + 1, 'reconnects after the 2 s backoff')
end)

test('bad login (CONNACK 5): backs off 30 s, never two connections at once', function()
	local d = boot({props = {['Password'] = 'wrong'}})
	d.C4:Advance(600)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	box(d, connack(5))
	truthy(d.C4.props['Last Error']:find('not authorized'))
	eq(#d.C4.net.disconnects, 1)
	d.env.OnConnectionStatusChanged(6001, 1883, 'OFFLINE') -- Director confirms the close
	d.C4:Advance(29000)
	eq(#d.C4.net.connects, 1, 'no retry inside the backoff')
	d.C4:Advance(1100)
	eq(#d.C4.net.connects, 2, 'retry after 30 s')
end)

test('backoff grows 2, 4, 8 s on repeated failures and resets on success', function()
	local d = boot()
	d.C4:Advance(600)
	local times = {}
	for i = 1, 3 do
		d.env.OnConnectionStatusChanged(6001, 1883, 'OFFLINE')
		local n = #d.C4.net.connects
		local waited = 0
		while (#d.C4.net.connects == n and waited < 70000) do
			d.C4:Advance(100); waited = waited + 100
		end
		times[i] = waited
	end
	truthy(times[1] >= 2000 and times[1] <= 2100, 'first ' .. times[1])
	truthy(times[2] >= 4000 and times[2] <= 4100, 'second ' .. times[2])
	truthy(times[3] >= 8000 and times[3] <= 8100, 'third ' .. times[3])
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	box(d, connack(0))
	eq(d.m.CLIENT.attempt, 0)
end)

test('connect timeout, stray ONLINE is closed, no CONNACK drops', function()
	local d = boot()
	d.C4:Advance(600)
	d.C4:Advance(11000)
	eq(#d.C4.net.disconnects, 1, 'gave up on a connect that never came ONLINE')
	eq(d.m.CLIENT.state, 'WAIT')
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE') -- late ONLINE while waiting
	eq(#d.C4.net.disconnects, 2, 'late socket closed')
	d.C4:Advance(3000)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	d.C4:Advance(11000)
	eq(d.m.CLIENT.state, 'WAIT', 'no CONNACK within 10 s')
end)

test('outgoing packets are spaced so the box never sees two in one read', function()
	local d = boot()
	connect(d)
	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(301, 'CLOSE', {})
	d.env.ReceivedFromProxy(302, 'CLOSE', {})
	d.env.ReceivedFromProxy(303, 'CLOSE', {})
	eq(#d.C4.net.sent - from + 1, 1, 'only the first goes out at once')
	d.C4:Advance(200)
	local pubs = publishes(d.C4, from)
	eq(#pubs, 3)
	truthy(pubs[2].at - pubs[1].at >= 40 and pubs[3].at - pubs[2].at >= 40, 'at least 40 ms apart')
end)

test('after an IR burst or serial string the next packet waits for the box to finish', function()
	local IO = probe.m.IO
	eq(IO.IrDurationMs('0000 006D 0000 0001 0010 0020'), 1)
	eq(IO.IrDurationMs('nec 0x04 0x08 3'), 440)
	local frame = {}
	for i = 1, 34 do frame[#frame + 1] = '0040' end -- 17 pairs of 64 periods
	local code = '0000 006D 0000 0011 ' .. table.concat(frame, ' ')
	local ms = IO.IrDurationMs(IO.BuildPronto(code, 3))
	eq(ms, math.floor(3 * 34 * 64 * 109 * 0.241246 / 1000))
	local d = boot()
	connect(d)
	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(101, 'PULSE', {data = '<pattern>' .. code .. '</pattern><repeatcount>3</repeatcount>'})
	d.env.ReceivedFromProxy(301, 'CLOSE', {})
	d.C4:Advance(1000)
	local ir, relay = find_pub(d.C4, 'zrp6/ir/1/send', from), find_pub(d.C4, 'zrp6/relay/1/set', from)
	truthy(relay.at - ir.at >= ms + 40, 'relay waited ' .. (relay.at - ir.at) .. ' ms for a ' .. ms .. ' ms burst')
	from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(201, 'SEND', {DATA = string.rep('x', 96)}) -- 96 bytes at 9600 = 100 ms
	d.env.ReceivedFromProxy(302, 'CLOSE', {})
	d.C4:Advance(1000)
	local tx, r2 = find_pub(d.C4, 'zrp6/serial/1/tx', from), find_pub(d.C4, 'zrp6/relay/2/set', from)
	truthy(r2.at - tx.at >= 140, 'serial hold ' .. (r2.at - tx.at))
end)

test('changing the box address reconnects to the new one', function()
	local d = boot()
	connect(d)
	setprop(d, 'Box IP Address', '127.0.0.2')
	local pk = sent_packets(d.C4)
	eq(pk[#pk].type, 14, 'DISCONNECT sent to the old box')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '127.0.0.2')
end)

test('a base topic too long for the box is reported', function()
	local d = boot({props = {['Base Topic'] = 'kitchen-zrp6'}})
	truthy((d.C4.props['Last Error'] or ''):find('too long'))
end)

-- Relays, triggers, contacts ---------------------------------------------------------------

test('relay and trigger state notify the RELAY bindings (STATE_* first, then changes)', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/relay/1/state', 'ON', true) .. box_publish('zrp6/trigger/2/state', 'OFF', true))
	eq(proxy_calls(d.C4, 301), 'STATE_CLOSED')
	eq(proxy_calls(d.C4, 312), 'STATE_OPENED')
	box(d, box_publish('zrp6/relay/1/state', 'ON', false))
	eq(proxy_calls(d.C4, 301), 'STATE_CLOSED', 'no repeat without a change')
	box(d, box_publish('zrp6/relay/1/state', 'OFF', false))
	eq(proxy_calls(d.C4, 301), 'STATE_CLOSED,OPENED')
	d.env.ReceivedFromProxy(301, 'GET_STATE', {})
	eq(proxy_calls(d.C4, 301), 'STATE_CLOSED,OPENED,STATE_OPENED')
	eq(d.C4.proxy[1].kind, 'NOTIFY')
end)

test('contact inputs notify the CONTACT_SENSOR bindings', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/sense/3/state', 'CLOSED', true))
	box(d, box_publish('zrp6/sense/3/state', 'OPEN', false))
	eq(proxy_calls(d.C4, 403), 'STATE_CLOSED,OPENED')
	d.env.OnBindingChanged(403, 'CONTACT_SENSOR', true, 99, 1)
	eq(proxy_calls(d.C4, 403), 'STATE_CLOSED,OPENED,STATE_OPENED', 'state re-sent when bound')
end)

test('OPEN / CLOSE / TOGGLE / TRIGGER from bound relay proxies publish /set', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/relay/2/state', 'ON', true))
	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(302, 'TOGGLE', {})
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/relay/2/set', from).payload, 'OFF', 'toggle of ON')
	from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(311, 'TRIGGER', {TIME = '250'})
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/trigger/1/set', from).payload, 'ON')
	local from2 = #d.C4.net.sent + 1
	d.C4:Advance(260)
	eq(find_pub(d.C4, 'zrp6/trigger/1/set', from2).payload, 'OFF', 'pulse ends after TIME')
	from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(303, 'OPEN', {})
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/relay/3/set', from).payload, 'OFF')
end)

-- IR ---------------------------------------------------------------------------------------------

test('IR_OUT bindings 101-107 are offered on first start', function()
	local d = boot()
	for n = 1, 7 do
		eq(d.C4.bindings[100 + n].class, 'IR_OUT')
	end
	eq(d.C4.bindings[107].name, 'IR Out 7 (HIGH OUT)')
	eq(d.C4.persist.ZRP6_IR_BINDINGS['3'], 103)
end)

test('PULSE from an IR device driver sends Pronto with the repeat unrolled', function()
	local d = boot()
	connect(d)
	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(102, 'PULSE', {data = '<ircode><id>10</id><transmit>PULSE</transmit><repeatcount>3</repeatcount>'
		.. '<delayafter>0</delayafter><pattern>0000 006d 0000 0002 0010 0020 0030 0040</pattern></ircode>'})
	d.C4:Advance(50)
	local p = find_pub(d.C4, 'zrp6/ir/2/send', from)
	eq(p.payload, '0000 006D 0006 0000 0010 0020 0030 0040 0010 0020 0030 0040 0010 0020 0030 0040')
	from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(102, 'STOP', {})
	d.C4:Advance(50)
	eq(#publishes(d.C4, from), 0, 'STOP is a no-op')
end)

test('Pronto: once-section kept, oversize reduces repeats, non-raw passes through', function()
	local IO = probe.m.IO
	eq(IO.BuildPronto('0000 0067 0001 0001 0001 0002 0003 0004', 2), '0000 0067 0003 0000 0001 0002 0003 0004 0003 0004')
	local frame = {}
	for i = 1, 34 do frame[#frame + 1] = '0015' end
	local code = '0000 006D 0000 0011 ' .. table.concat(frame, ' ')
	local out = IO.BuildPronto(code, 10, 700)
	truthy(#out <= 700, 'fits')
	eq(out:sub(1, 15), '0000 006D 0044 ', '4 repeats of 17 pairs (20 + 4 x 170 bytes) fit in 700')
	eq(IO.BuildPronto('0100 0067 0000 0001 0001 0002', 3), '0100 0067 0000 0001 0001 0002')
	eq(IO.BuildPronto('garbage', 3), 'garbage')
end)

-- Serial -----------------------------------------------------------------------------------------

test('serial data from a bound device goes to serial/<n>/tx, long data in 256-byte parts', function()
	local d = boot()
	connect(d)
	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(201, 'SEND', {DATA = 'PWR ON\r'})
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/serial/1/tx', from).payload, 'PWR ON\r')
	from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(202, 'SEND', {DATA = string.rep('z', 600)})
	d.C4:Advance(1000) -- each 256-byte part holds the queue ~267 ms at 9600
	local parts = {}
	for _, p in ipairs(publishes(d.C4, from)) do if (p.topic == 'zrp6/serial/2/tx') then parts[#parts + 1] = #p.payload end end
	eq(table.concat(parts, ','), '256,256,88')
end)

test('unknown proxy commands are logged once to a property', function()
	local d = boot()
	d.env.ReceivedFromProxy(201, 'WEIRD_THING', {X = '1'})
	truthy((d.C4.props['Last Unhandled Proxy Command'] or ''):find('WEIRD_THING'))
end)

test('serial rx sets the variable and fires the event', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/serial/2/rx', 'PWR=ON\r\n', false))
	eq(d.C4.vars.SERIAL_2_RX, 'PWR=ON')
	eq(d.C4.events.fired[#d.C4.events.fired], 'RS-232 2 Data Received')
end)

test('RS-232 config follows the bound device driver serialsettings', function()
	local d = boot({driverConfig = {[55] = '<serialsettings>9600 8 even 1 none</serialsettings>'}})
	connect(d)
	box(d, box_publish('zrp6/serial/1/config', '9600 8N1', true) .. box_publish('zrp6/serial/2/config', '9600 8N1', true))
	for n = 1, 7 do box(d, box_publish('zrp6/ir/' .. n .. '/mode', 'ir', true)) end
	d.C4:Advance(2000) -- settle: everything matches, nothing to send
	eq(find_pub(d.C4, 'zrp6/serial/1/config/set'), nil)
	eq(find_pub(d.C4, 'zrp6/ir/1/mode/set'), nil)
	d.env.OnBindingChanged(201, 'RS_232', true, 55, 1)
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/serial/1/config/set').payload, '9600 8E1')
	setprop(d, 'RS-232 2 Baud', '19200')
	setprop(d, 'RS-232 2 Format', '8N2')
	d.C4:Advance(200)
	local last
	for _, p in ipairs(publishes(d.C4)) do if (p.topic == 'zrp6/serial/2/config/set') then last = p.payload end end
	eq(last, '19200 8N2')
end)

-- IR port mode switching -------------------------------------------------------------------------

test('switching IR 3 to serial swaps IR_OUT 103 for RS_232 113 and publishes the mode', function()
	local d = boot()
	connect(d)
	for n = 1, 7 do box(d, box_publish('zrp6/ir/' .. n .. '/mode', 'ir', true)) end
	d.C4:Advance(2000)
	d.props['IR 3 Serial Baud'] = '19200'
	d.props['IR 3 Serial Polarity'] = 'TTL (idle high)'
	setprop(d, 'IR 3 Mode', 'Serial (one-way)')
	eq(d.C4.bindings[103], nil, 'IR_OUT removed')
	eq(d.C4.bindings[113].class, 'RS_232')
	eq(d.C4.propAttribs['IR 3 Serial Baud'], 0, 'baud shown')
	eq(d.C4.persist.ZRP6_IR_BINDINGS['3'], 113)
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/ir/3/mode/set').payload, 'serial 19200 ttl')
	box(d, box_publish('zrp6/ir/3/mode', 'serial 19200 ttl', true))
	truthy(d.C4.props['Box Port Settings']:find('IR3: serial 19200 ttl', 1, true))

	local from = #d.C4.net.sent + 1
	d.env.ReceivedFromProxy(113, 'SEND', {DATA = 'PWR\r'})
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/ir/3/tx', from).payload, 'PWR\r')

	from = #d.C4.net.sent + 1
	setprop(d, 'IR 3 Mode', 'IR')
	eq(d.C4.bindings[113], nil); eq(d.C4.bindings[103].class, 'IR_OUT')
	eq(d.C4.propAttribs['IR 3 Serial Baud'], 1, 'baud hidden')
	d.C4:Advance(50)
	eq(find_pub(d.C4, 'zrp6/ir/3/mode/set', from).payload, 'ir')
end)

test('dynamic bindings are restored in OnDriverInit from persisted state', function()
	local d = boot({persist = {ZRP6_IR_BINDINGS = {['1'] = 101, ['3'] = 113}}, props = {['IR 3 Mode'] = 'Serial (one-way)'}})
	eq(d.C4.bindingLog[1][1], 'add'); eq(d.C4.bindingLog[1][2], 101)
	eq(d.C4.bindingLog[2][2], 113, 'serial binding restored before LateInit')
	eq(d.C4.bindings[113].class, 'RS_232')
	for _, e in ipairs(d.C4.bindingLog) do
		truthy(not (e[1] == 'remove' and e[2] == 113), '113 kept')
	end
end)

local function sets(C4, from)
	local out = {}
	for _, p in ipairs(publishes(C4, from)) do
		if (p.topic:find('/set$')) then out[#out + 1] = p.topic .. '=' .. p.payload end
	end
	return table.concat(out, ' ')
end

test('box wins: on reconnect the box\'s port settings are adopted and nothing is published', function()
	local d = boot({props = {['IR 3 Mode'] = 'Serial (one-way)', ['IR 3 Serial Baud'] = '19200',
		['RS-232 1 Baud'] = '19200', ['RS-232 1 Format'] = '8E1'}})
	eq(d.C4.bindings[113].class, 'RS_232')
	connect(d)
	local retained = box_publish('zrp6/ir/3/mode', 'ir', true) .. box_publish('zrp6/ir/5/mode', 'serial 38400 ttl', true)
		.. box_publish('zrp6/serial/1/config', '9600 8N1', true)
	for _, n in ipairs({1, 2, 4, 6, 7}) do retained = retained .. box_publish('zrp6/ir/' .. n .. '/mode', 'ir', true) end
	box(d, retained)
	d.C4:Advance(30000)
	eq(sets(d.C4), '', 'nothing sent back to the box')
	eq(d.props['IR 3 Mode'], 'IR'); eq(d.C4.props['IR 3 Mode'], 'IR')
	eq(d.props['IR 5 Mode'], 'Serial (one-way)'); eq(d.props['IR 5 Serial Baud'], '38400'); eq(d.props['IR 5 Serial Polarity'], 'TTL (idle high)')
	eq(d.props['RS-232 1 Baud'], '9600'); eq(d.props['RS-232 1 Format'], '8N1')
	eq(d.C4.bindings[113], nil); eq(d.C4.bindings[103].class, 'IR_OUT', 'connections follow the box')
	eq(d.C4.bindings[105], nil); eq(d.C4.bindings[115].class, 'RS_232')
	-- Director echoing the driver's own property writes is not an edit.
	for _, name in ipairs({'IR 3 Mode', 'IR 5 Mode', 'IR 5 Serial Baud', 'IR 5 Serial Polarity', 'RS-232 1 Baud', 'RS-232 1 Format'}) do
		d.env.OnPropertyChanged(name)
	end
	d.C4:Advance(1000)
	eq(sets(d.C4), '', 'echoes do not publish')
	-- A second reconnect with the same box values: still nothing.
	d.env.OnConnectionStatusChanged(6001, 1883, 'OFFLINE')
	d.C4:Advance(2100)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	box(d, connack(0)); d.C4:Advance(100); box(d, retained)
	d.C4:Advance(30000)
	eq(sets(d.C4), '')
end)

test('box wins: an installer\'s edit publishes exactly once, then the box\'s answer is adopted quietly', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/ir/2/mode', 'ir', true) .. box_publish('zrp6/serial/2/config', '9600 8N1', true))
	d.props['IR 2 Serial Baud'] = '4800'
	setprop(d, 'IR 2 Mode', 'Serial (one-way)')
	d.env.OnPropertyChanged('IR 2 Mode') -- Composer can fire twice
	setprop(d, 'RS-232 2 Format', '7E1')
	setprop(d, 'RS-232 2 Format', '7E1')
	d.C4:Advance(500)
	eq(sets(d.C4), 'zrp6/ir/2/mode/set=serial 4800 rs232 zrp6/serial/2/config/set=9600 7E1')
	eq(d.C4.bindings[112].class, 'RS_232')
	local from = #d.C4.net.sent + 1
	box(d, box_publish('zrp6/ir/2/mode', 'serial 4800 rs232', true) .. box_publish('zrp6/serial/2/config', '9600 7E1', true))
	d.C4:Advance(5000)
	eq(sets(d.C4, from), '', 'the box agreeing sends nothing')
	setprop(d, 'IR 2 Serial Baud', '4800') -- same as the box
	d.C4:Advance(500)
	eq(sets(d.C4, from), '', 'an edit to what the box already has sends nothing')
end)

test('box wins: port settings changed on the box while connected are adopted', function()
	local d = boot()
	connect(d)
	box(d, box_publish('zrp6/ir/4/mode', 'ir', true))
	box(d, box_publish('zrp6/ir/4/mode', 'serial 9600 rs232', false))
	eq(d.props['IR 4 Mode'], 'Serial (one-way)'); eq(d.props['IR 4 Serial Baud'], 'Auto (bound device)', 'Auto already gives 9600')
	eq(d.C4.bindings[114].class, 'RS_232'); eq(d.C4.bindings[104], nil)
	eq(d.C4.propAttribs['IR 4 Serial Baud'], 0, 'baud shown')
	truthy(d.C4.props['Box Port Settings']:find('IR4: serial 9600 rs232', 1, true))
	eq(sets(d.C4), '')
end)

test('Auto baud is pushed only when a bound device\'s settings become known, never on reconnect', function()
	local d = boot({props = {['IR 5 Mode'] = 'Serial (one-way)'},
		driverConfig = {[77] = '<config><serialsettings>38400 8 none 1 none</serialsettings></config>'}})
	connect(d)
	box(d, box_publish('zrp6/ir/5/mode', 'serial 9600 rs232', true))
	d.C4:Advance(30000)
	eq(sets(d.C4), '', 'Auto with nothing bound: nothing on reconnect')
	eq(d.props['IR 5 Serial Baud'], 'Auto (bound device)', 'Auto kept: it already gives 9600')
	d.env.OnBindingChanged(115, 'RS_232', true, 77, 1)
	d.C4:Advance(100)
	eq(sets(d.C4), 'zrp6/ir/5/mode/set=serial 38400 rs232')
	local from = #d.C4.net.sent + 1
	box(d, box_publish('zrp6/ir/5/mode', 'serial 38400 rs232', true))
	eq(d.props['IR 5 Serial Baud'], 'Auto (bound device)')
	d.env.OnConnectionStatusChanged(6001, 1883, 'OFFLINE')
	d.C4:Advance(2100)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	box(d, connack(0)); d.C4:Advance(100)
	box(d, box_publish('zrp6/ir/5/mode', 'serial 38400 rs232', true))
	d.C4:Advance(30000)
	eq(sets(d.C4, from), '', 'nothing on reconnect')
end)

-- Remotes ----------------------------------------------------------------------------------------

test('remote codes: bare codes are remote "" (the bench remote), p-prefixed codes name their remote', function()
	local R = probe.m.REMOTES
	local id, b = R.Parse('14.0.2.8.0.2.9')
	eq(id, ''); eq(b, '14.0.2.8.0.2.9')
	id, b = R.Parse('p16.16.14.12.15:14.0.2.8')
	eq(id, 'p16.16.14.12.15'); eq(b, '14.0.2.8')
end)

local function mapped_driver()
	local d = boot({
		devices = {
			[27] = {driverFileName = 'control4_sr150.c4i', deviceName = 'System Remote Control SR-150', roomId = '10', roomName = 'Den'},
			[17] = {driverFileName = 'control4_sr260.c4i', deviceName = 'System Remote Control SR260', roomId = '11', roomName = 'Kitchen'},
		},
		sources = {[11] = {watch = {77}, listen = {88}}},
	})
	connect(d)
	return d
end

test('learn flow through the Remote Buttons tab, two remotes, room actions run on press', function()
	local d = mapped_driver()
	local r = ui(d, 'UI_LEARN', '{"name":"Power"}')
	eq(r._command, 'STATE_DATA'); eq(r.data.learn.armed, true)
	box(d, box_publish('zrp6/remote/code', '14.0.2.8.0.2.9', false))
	local btn = d.m.REMOTES.data.buttons['14.0.2.8.0.2.9']
	truthy(btn, 'learned'); eq(btn.name, 'Power'); eq(btn.remote, '')
	eq(d.C4.events.added[btn.event], 'Button: Remote 1 / Power')
	eq(#d.C4.deviceCalls, 0, 'learning does not run anything')

	ui(d, 'UI_LEARN', '{}')
	box(d, box_publish('zrp6/remote/code', 'p16.16.14.12.15:14.0.2.8', false))
	local b2 = d.m.REMOTES.data.buttons['p16.16.14.12.15:14.0.2.8']
	eq(b2.remote, 'p16.16.14.12.15'); eq(b2.name, 'Button 1')
	eq(d.m.REMOTES.RemoteName('p16.16.14.12.15'), 'Remote 2')

	r = ui(d, 'UI_SAVE_BUTTON', '{"code":"14.0.2.8.0.2.9","name":"Power","actions":['
		.. '{"kind":"command","room":10,"roomName":"Den","command":"START_VOL_UP","label":"Volume Up","params":{}},'
		.. '{"kind":"source","room":11,"roomName":"Kitchen","source":77,"label":"Apple TV"},'
		.. '{"kind":"command","room":10,"command":"bad command!"}]}')
	eq(r._command, 'STATE_DATA')
	eq(pushed(d, 'SAVED').code, '14.0.2.8.0.2.9')
	eq(#btn.actions, 2, 'invalid action dropped')
	eq(btn.actions[1].release, 'STOP_VOL_UP', 'START_ gets its STOP_: a remote press is a tap')

	d.C4.deviceCalls = {}
	d.C4.events.fired = {}
	box(d, box_publish('zrp6/remote/code', '14.0.2.8.0.2.9', false))
	local calls = {}
	for _, c in ipairs(d.C4.deviceCalls) do
		if (not c.command:find('^GET_')) then calls[#calls + 1] = c.id .. ':' .. c.command end
	end
	eq(table.concat(calls, ' '), '10:START_VOL_UP 10:STOP_VOL_UP 11:SELECT_VIDEO_DEVICE')
	eq(d.C4.deviceCalls[#d.C4.deviceCalls].params.deviceid, 77)
	eq(table.concat(d.C4.events.fired, '|'), 'Remote Button Pressed|Button: Remote 1 / Power')
	eq(d.C4.vars.LAST_REMOTE_BUTTON, 'Power')

	box(d, box_publish('zrp6/remote/code', '1.2.3', false))
	eq(d.C4.events.fired[#d.C4.events.fired], 'Unmapped Remote Button Pressed')
end)

test('mappings survive a restart (PersistSetValue)', function()
	local d = mapped_driver()
	ui(d, 'UI_SAVE_BUTTON', '{"code":"p9:1.1","name":"Off","actions":[{"room":10,"action":"ROOM_OFF"}]}')
	local d2 = boot({persist = d.C4.persist})
	connect(d2)
	eq(d2.C4.events.added[1000], 'Button: Remote 1 / Off', 'event re-registered')
	box(d2, box_publish('zrp6/remote/code', 'p9:1.1', false))
	eq(d2.C4.deviceCalls[1].command, 'ROOM_OFF')
	eq(d2.C4.deviceCalls[1].id, 10)
end)

test('rename remote, delete button, command parameters are replayed as given', function()
	local d = mapped_driver()
	ui(d, 'UI_SAVE_BUTTON', '{"code":"p9:1.1","name":"Vol","actions":[{"kind":"command","room":10,"command":"SET_VOLUME_LEVEL",'
		.. '"params":{"LEVEL":40},"label":"Set Volume Level"},{"kind":"command","room":10,"command":"playpause"}]}')
	ui(d, 'UI_RENAME_REMOTE', '{"id":"p9","name":"Bedroom"}')
	eq(d.C4.events.added[1000], 'Button: Bedroom / Vol')
	box(d, box_publish('zrp6/remote/code', 'p9:1.1', false))
	eq(d.C4.deviceCalls[1].command, 'SET_VOLUME_LEVEL'); eq(d.C4.deviceCalls[1].params.LEVEL, '40'); eq(d.C4.deviceCalls[1].id, 10)
	eq(d.C4.deviceCalls[2].command, 'PLAYPAUSE')
	eq(pushed(d, 'STATE_DATA').buttons[1].summary, 'Set Volume Level (LEVEL=40) in Den; PLAYPAUSE in Den')
	ui(d, 'UI_DELETE_BUTTON', '{"code":"p9:1.1"}')
	eq(d.m.REMOTES.data.buttons['p9:1.1'], nil)
	eq(d.C4.events.deleted[1], 1000)
	eq(d.m.REMOTES.data.remotes.p9, nil, 'empty remote forgotten')
end)

test('learn times out', function()
	local d = mapped_driver()
	ui(d, 'UI_LEARN', '{"seconds":5}')
	d.C4:Advance(6500)
	eq(d.m.REMOTES.learn.armed, false)
	eq(pushed(d, 'STATE_DATA').learn.armed, false, 'the tab is told')
end)

-- Mapper HTTP server ---------------------------------------------------------------------------------

test('Remote Buttons tab: state, rooms and sources come back as SendDataToUI pushes', function()
	local d = mapped_driver()
	d.C4.dataToUI = {}
	local r = ui(d, 'UI_GET_STATE')
	eq(r._command, 'STATE_DATA', 'REST answer carries the command')
	local st = pushed(d, 'STATE_DATA')
	eq(st.connected, true); eq(st.actions, nil, 'actions come from Director, not a fixed list'); eq(#st.remotes, 0)
	ui(d, 'UI_GET_ROOMS')
	local rooms = pushed(d, 'ROOMS_DATA').rooms
	eq(rooms[1].name, 'Den'); eq(rooms[2].id, 11)
	ui(d, 'UI_GET_SOURCES', '{"room":11}')
	local src = pushed(d, 'SOURCES_DATA')
	eq(src.room, 11); eq(#src.sources, 2); eq(src.sources[1].type, 'watch'); eq(src.sources[2].id, 88)
	eq(d.env.UIRequest('SOMETHING_ELSE', {}), nil, 'not ours')
	r = ui(d, 'UI_SAVE_BUTTON', '{"name":"x"}')
	eq(r._command, 'ERROR'); eq(pushed(d, 'ERROR').message, 'code is required')
	r = ui(d, 'UI_TEST_BUTTON', '{"code":"nope"}')
	eq(r._command, 'ERROR')
	r = d.m.JSON.Decode(d.env.UIRequest('UI_GET_STATE', {DATA = 'bm90IGpzb24='})) -- "not json": falls back to tParams
	eq(r._command, 'STATE_DATA')
end)

test('the tab is pushed a fresh state on presses, learning and connection changes', function()
	local d = mapped_driver()
	d.C4.dataToUI = {}
	box(d, box_publish('zrp6/remote/code', 'p5:1.1', false))
	local st = pushed(d, 'STATE_DATA')
	eq(st.recent[1].code, 'p5:1.1'); eq(st.recent[1].known, false); eq(st.recent[1].remote, 'p5')
	ui(d, 'UI_LEARN', '{"name":"Play"}')
	box(d, box_publish('zrp6/remote/code', '3.3.3', false))
	st = pushed(d, 'STATE_DATA')
	eq(st.learn.armed, false); eq(st.learn.captured, '3.3.3')
	eq(st.remotes[1].id, '', 'bench remote id is empty'); eq(st.buttons[1].name, 'Play')
	d.env.OnConnectionStatusChanged(6001, 1883, 'OFFLINE')
	eq(pushed(d, 'STATE_DATA').connected, false)
end)

test('mappings saved by 1.0.0 (remote "default", remotes keyed by id) still load', function()
	local old = {remotes = {default = {id = 'default', name = 'Bench'}, p7 = {id = 'p7', name = 'Den remote'}},
		buttons = {['1.2'] = {code = '1.2', remote = 'default', name = 'Off', event = 1000, actions = {{room = 10, action = 'ROOM_OFF'}}},
			['p7:4'] = {code = 'p7:4', remote = 'p7', name = 'Mute', event = 1001, actions = {}}},
		nextEvent = 1002}
	local d2 = boot({persist = {ZRP6_REMOTES = old}})
	eq(d2.m.REMOTES.RemoteName(''), 'Bench')
	eq(d2.m.REMOTES.data.buttons['1.2'].remote, '')
	eq(d2.C4.events.added[1000], 'Button: Bench / Off')
	d2.m.REMOTES.Save()
	local saved = d2.C4.persist.ZRP6_REMOTES
	eq(saved.version, 2); eq(#saved.remotes, 2, 'remotes stored as a list')
end)

test('fallback rooms come from C4:GetDevices, sources from the room UI request', function()
	local d = mapped_driver()
	local rooms = d.m.ROOMS.List(true)
	eq(#rooms, 2); eq(rooms[1].name, 'Den'); eq(rooms[1].id, 10); eq(rooms[2].name, 'Kitchen')
	local src = d.m.ROOMS.Sources(11)
	eq(src[1].id, 77); eq(src[1].type, 'watch'); eq(src[2].id, 88); eq(src[2].type, 'listen')
end)

test('mappings saved by 1.0.0 as fixed action ids still run, as room commands', function()
	local old = {remotes = {{id = '', name = 'Bench'}}, nextEvent = 1001,
		buttons = {['1.2'] = {code = '1.2', name = 'Vol', event = 1000, actions = {
			{room = 10, action = 'VOLUME_UP'}, {room = 10, action = 'SET_VOLUME', level = 30}, {action = 'EVENT_ONLY'}, {action = 'NOPE'}}}}}
	local d = boot({persist = {ZRP6_REMOTES = old}, devices = {}})
	local b = d.m.REMOTES.data.buttons['1.2']
	eq(#b.actions, 3)
	eq(b.actions[1].kind, 'command'); eq(b.actions[1].command, 'START_VOL_UP'); eq(b.actions[1].release, 'STOP_VOL_UP')
	eq(b.actions[2].command, 'SET_VOLUME_LEVEL'); eq(b.actions[2].params.LEVEL, '30')
	eq(b.actions[3].kind, 'event')
	connect(d)
	box(d, box_publish('zrp6/remote/code', '1.2', false))
	local t = {}
	for _, c in ipairs(d.C4.deviceCalls) do t[#t + 1] = c.command end
	eq(table.concat(t, ' '), 'START_VOL_UP STOP_VOL_UP SET_VOLUME_LEVEL')
end)

test('client ID starts with ! (a priority client on the box)', function()
	local d = boot()
	d.C4:Advance(600)
	d.env.OnConnectionStatusChanged(6001, 1883, 'ONLINE')
	local body = sent_packets(d.C4)[1].body
	local idlen = body:byte(11) * 256 + body:byte(12)
	eq(body:sub(13, 12 + idlen), '!c4-zrp6-4242')
end)

test('Connection Mode: Central broker uses the broker properties and stops mDNS; P2P brings it back', function()
	local d = boot({props = {['Box IP Address'] = '10.0.0.112', ['Connection Mode'] = 'Central broker',
		['Broker Host'] = 'broker.lan', ['Broker Port'] = '1884', ['Broker Username'] = 'c4', ['Broker Password'] = 'pw',
		['Broker Base Topic'] = 'zrp6'}})
	eq(d.C4.propAttribs['Box'], 1, 'box properties hidden'); eq(d.C4.propAttribs['Broker Host'], 0, 'broker properties shown')
	eq(#d.C4.udpServers, 0, 'no mDNS listener in broker mode')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, 'broker.lan'); eq(d.C4.net.connects[1].port, 1884)
	d.env.OnConnectionStatusChanged(6001, 1884, 'ONLINE')
	local body = sent_packets(d.C4)[1].body
	truthy(body:find(mstr('c4') .. mstr('pw'), 1, true), 'broker credentials in CONNECT')
	truthy(body:find(mstr('!c4-zrp6-4242'), 1, true), 'priority client ID here too')
	box(d, connack(0))
	d.C4:Advance(100)
	box(d, box_publish('zrp6/relay/1/state', 'ON', true))
	eq(proxy_calls(d.C4, 301), 'STATE_CLOSED', 'topics and bindings the same through a broker')
	setprop(d, 'Connection Mode', 'Direct to box (P2P)')
	eq(d.C4.propAttribs['Box'], 0); eq(d.C4.propAttribs['Broker Host'], 1)
	eq(#d.C4.udpServers, 1, 'mDNS listening again')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.112')
	local b2 = boot({props = {['Connection Mode'] = 'Central broker', ['Broker Host'] = ''}})
	truthy(b2.C4.props['Connection Status']:find('set Broker Host', 1, true))
end)

-- mDNS discovery -----------------------------------------------------------------------------------

-- The box's announcement, built byte by byte from the firmware's description:
-- ID 0, flags 0x8400, 0 questions, 4 answers, TTL 120; PTR, SRV (cache-flush),
-- TXT (cache-flush), A (cache-flush), with name compression.
local function dnsname(...)
	local out = {}
	for _, l in ipairs({...}) do out[#out + 1] = string.char(#l) .. l end
	return table.concat(out)
end
local function ptr(off) return string.char(0xC0 + math.floor(off / 256), off % 256) end
local function u32(n) return u16(math.floor(n / 65536)) .. u16(n % 65536) end
local function announcement(mac, ip, ttl)
	ttl = ttl or 120
	local inst = 'zrp6-' .. mac:sub(9, 12)
	local a, b, c, d = ip:match('^(%d+)%.(%d+)%.(%d+)%.(%d+)$')
	local p = u16(0) .. u16(0x8400) .. u16(0) .. u16(4) .. u16(0) .. u16(0)
	-- offset 12: _zrp6._tcp.local (_zrp6 at 12, _tcp at 18, local at 23)
	p = p .. dnsname('_zrp6', '_tcp', 'local') .. '\0'
	local rdata = dnsname(inst) .. ptr(12)
	p = p .. u16(12) .. u16(1) .. u32(ttl) .. u16(#rdata)
	local offInst = #p
	p = p .. rdata
	local target = dnsname(inst) .. ptr(23)
	p = p .. ptr(offInst) .. u16(33) .. u16(0x8001) .. u32(ttl) .. u16(6 + #target) .. u16(0) .. u16(0) .. u16(1883)
	local offHost = #p
	p = p .. target
	local txt = 'mac=' .. mac
	p = p .. ptr(offInst) .. u16(16) .. u16(0x8001) .. u32(ttl) .. u16(1 + #txt) .. string.char(#txt) .. txt
	p = p .. ptr(offHost) .. u16(1) .. u16(0x8001) .. u32(ttl) .. u16(4) .. string.char(tonumber(a), tonumber(b), tonumber(c), tonumber(d))
	return p
end
local function tohex(s) return (s:gsub('.', function(c) return string.format('%02x', c:byte()) end)) end

-- The same packet as agreed with the firmware side (MAC 025A52D17A36, 10.0.0.112).
local AGREED_HEX = '000084000000000400000000055f7a727036045f746370056c6f63616c00000c000100000078000c097a7270362d37413336c00c'
	.. 'c0280021800100000078001200000000075b097a7270362d37413336c017c02800108001000000780011106d61633d30323541'
	.. '3532443137413336c046000180010000007800040a000070'

test('mDNS: the box announcement is the agreed bytes and parses', function()
	local pkt = announcement('025A52D17A36', '10.0.0.112')
	eq(tohex(pkt), AGREED_HEX)
	eq(#pkt, 127)
	local boxes = probe.m.DISCO.Parse(pkt)
	eq(#boxes, 1)
	local b = boxes[1]
	eq(b.name, 'zrp6-7A36'); eq(b.host, 'zrp6-7A36.local'); eq(b.mac, '025A52D17A36'); eq(b.ip, '10.0.0.112'); eq(b.port, 1883)
end)

test('mDNS: uncompressed and upper-case names parse the same; other traffic is ignored', function()
	local D = probe.m.DISCO
	local full = u16(0) .. u16(0x8400) .. u16(0) .. u16(3) .. u16(0) .. u16(0)
		.. dnsname('_ZRP6', '_TCP', 'LOCAL') .. '\0' .. u16(12) .. u16(1) .. u32(120) .. u16(28) .. dnsname('zrp6-0001', '_zrp6', '_tcp', 'local') .. '\0'
		.. dnsname('zrp6-0001', '_zrp6', '_tcp', 'local') .. '\0' .. u16(16) .. u16(0x8001) .. u32(120) .. u16(17) .. '\16mac=aabbccdd0001'
		.. dnsname('zrp6-0001', 'local') .. '\0' .. u16(1) .. u16(0x8001) .. u32(120) .. u16(4) .. '\192\168\1\9'
	local b = D.Parse(full)[1]
	truthy(b, 'parsed'); eq(b.mac, 'AABBCCDD0001'); eq(b.ip, '192.168.1.9'); eq(b.host, 'zrp6-0001.local'); eq(b.port, 1883)
	local good = announcement('025A52D17A36', '10.0.0.112')
	eq(#D.Parse(good:sub(1, 2) .. u16(0) .. good:sub(5)), 0, 'a query is not an announcement')
	eq(#D.Parse(announcement('025A52D17A36', '10.0.0.112', 0)), 0, 'TTL 0 is a goodbye')
	eq(#D.Parse((good:gsub('_zrp6', '_http'))), 0, 'another service')
	for n = 0, #good - 1 do
		D.Parse(good:sub(1, n)) -- truncated anywhere: no error
	end
	local loop = u16(0) .. u16(0x8400) .. u16(0) .. u16(1) .. u16(0) .. u16(0) .. ptr(12)
	eq(#D.Parse(loop), 0, 'pointer loop')
end)

local function mdns(d, pkt)
	d.env.OnServerDataIn(1, pkt, '10.0.0.112', 5353, 'zrp6-mdns')
end

test('discovery: with no address typed, the first box heard is used and remembered', function()
	local d = boot({props = {['Box IP Address'] = ''}})
	eq(d.C4.udpServers[1].port, 5353); eq(d.C4.udpServers[1].udp, true)
	d.C4:Advance(1000)
	eq(#d.C4.net.connects, 0, 'nothing to connect to yet')
	truthy(d.C4.props['Connection Status']:find('Waiting for a box'))
	mdns(d, announcement('025A52D17A36', '10.0.0.112'))
	eq(d.C4.persist.ZRP6_BOX.mac, '025A52D17A36')
	truthy(d.C4.props['Selected Box']:find('zrp6-7A36 (025A52D17A36) at 10.0.0.112', 1, true))
	eq(d.C4.propLists.Box.default, 'zrp6-7A36 (025A52D17A36) at 10.0.0.112')
	eq(d.C4.props['Box Address In Use'], '10.0.0.112:1883 (discovered)')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.112')
	eq(#d.C4.net.connects, 1)
end)

test('discovery: the chosen box moving to a new address is followed', function()
	local d = boot({props = {['Box IP Address'] = ''}})
	mdns(d, announcement('025A52D17A36', '10.0.0.112'))
	connect(d)
	mdns(d, announcement('025A52D17A36', '10.0.0.112'))
	eq(#d.C4.net.connects, 1, 'same address: nothing happens')
	mdns(d, announcement('025A52D17A36', '10.0.0.130'))
	local pk = sent_packets(d.C4)
	eq(pk[#pk].type, 14, 'DISCONNECT to the old address')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.130')
	eq(#d.C4.net.connects, 2)
	eq(d.C4.persist.ZRP6_BOX.ip, '10.0.0.130')
end)

test('discovery: Box IP Address overrides it; clearing it goes back to the discovered box', function()
	local d = boot({props = {['Box IP Address'] = ''}})
	mdns(d, announcement('025A52D17A36', '10.0.0.112'))
	d.C4:Advance(600)
	setprop(d, 'Box IP Address', 'zrp6-7A36.local')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, 'zrp6-7A36.local')
	eq(d.C4.props['Box Address In Use'], 'zrp6-7A36.local:1883 (Box IP Address)')
	local n = #d.C4.net.created
	mdns(d, announcement('025A52D17A36', '10.0.0.131'))
	d.C4:Advance(600)
	eq(#d.C4.net.created, n, 'the typed address wins over a moved box')
	setprop(d, 'Box IP Address', '')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.131', 'back to discovery, at the newest address')
end)

test('discovery: after a restart the remembered box is dialled before any announcement', function()
	local d = boot({props = {['Box IP Address'] = ''}, persist = {ZRP6_BOX = {mac = '025A52D17A36', name = 'zrp6-7A36', ip = '10.0.0.140', port = 1883}}})
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.140')
	truthy(d.C4.props['Selected Box']:find('not heard since the driver started', 1, true))
end)

test('discovery: two boxes are listed; picking one switches to it; Forget clears the choice', function()
	local d = boot({props = {['Box IP Address'] = ''}})
	mdns(d, announcement('025A52D17A36', '10.0.0.112'))
	mdns(d, announcement('025A52AABBCC', '10.0.0.150'))
	eq(d.C4.persist.ZRP6_BOX.mac, '025A52D17A36', 'first one heard stays chosen')
	local list = d.C4.propLists.Box.list
	truthy(list:find('zrp6-BBCC (025A52AABBCC) at 10.0.0.150', 1, true) and list:find('zrp6-7A36', 1, true), list)
	setprop(d, 'Box', 'zrp6-BBCC (025A52AABBCC) at 10.0.0.150')
	d.C4:Advance(600)
	eq(d.C4.net.created[#d.C4.net.created].address, '10.0.0.150')
	d.env.ExecuteCommand('LUA_ACTION', {ACTION = 'FORGET_SELECTED_BOX'})
	eq(d.C4.props['Selected Box'], 'None chosen')
	eq(d.C4.persist.ZRP6_BOX.mac, nil)
end)

test('discovery: without a UDP server the network binding listens (MIRROR_UDP_PORT); Auto swaps a silent listener', function()
	local d = boot({props = {['Box IP Address'] = ''}, serverResult = false})
	local opts
	for _, e in ipairs(d.C4.net.other) do
		if (e.what == 'options') then opts = e end
	end
	eq(opts.binding, 6002); eq(opts.port, 5353); eq(opts.options.MIRROR_UDP_PORT, true)
	eq(d.C4.net.other[#d.C4.net.other].what, 'connects'); eq(d.C4.net.other[#d.C4.net.other].kind, 'UDP')
	d.env.ReceivedFromNetwork(6002, 5353, announcement('025A52D17A36', '10.0.0.112'))
	eq(d.C4.persist.ZRP6_BOX.mac, '025A52D17A36')

	local d2 = boot({props = {['Box IP Address'] = ''}})
	eq(#d2.C4.net.other, 0, 'server first')
	d2.C4:Advance(181000)
	eq(d2.C4.udpServers[1].live, false, 'silent server dropped')
	eq(d2.C4.net.other[#d2.C4.net.other].what, 'connects', 'binding tried instead')
	truthy(d2.C4.props['Discovery Status']:find('UDP binding', 1, true))

	local d3 = boot({props = {['Box IP Address'] = ''}})
	d3.env.OnServerStatusChanged(5353, 'OFFLINE', 'zrp6-mdns')
	eq(d3.C4.net.other[#d3.C4.net.other].what, 'connects', 'server went offline -> binding')
end)

-- Programming commands ----------------------------------------------------------------------------------

test('programming commands: Send Serial with escapes, Set Output pulse, Send IR, Press Remote Button', function()
	local d = mapped_driver()
	local from = #d.C4.net.sent + 1
	d.env.ExecuteCommand('Send Serial', {Port = 'RS-232 2', Data = 'PWR\\r\\x01'})
	d.env.ExecuteCommand('Send Serial', {Port = 'IR 4', Data = 'x'}) -- IR 4 is in IR mode: ignored
	d.env.ExecuteCommand('Set Output', {Output = 'Trigger 2', State = 'Pulse', ['Time (ms)'] = '100'})
	d.env.ExecuteCommand('Send IR', {Port = '6', Code = 'nec 0x04 0x08', Repeats = '1'})
	d.C4:Advance(300)
	eq(find_pub(d.C4, 'zrp6/serial/2/tx', from).payload, 'PWR\r\1')
	eq(find_pub(d.C4, 'zrp6/ir/4/tx', from), nil)
	local trig = {}
	for _, p in ipairs(publishes(d.C4, from)) do if (p.topic == 'zrp6/trigger/2/set') then trig[#trig + 1] = p.payload end end
	eq(table.concat(trig, ','), 'ON,OFF')
	eq(find_pub(d.C4, 'zrp6/ir/6/send', from).payload, 'nec 0x04 0x08')
	ui(d, 'UI_SAVE_BUTTON', '{"code":"7.7","name":"Off","actions":[{"room":10,"action":"ROOM_OFF"}]}')
	local list = d.env.GetRemoteButtonList('')
	eq(list[1].value, '7.7')
	d.C4.deviceCalls = {}
	d.env.ExecuteCommand('Press Remote Button', {Button = '7.7'})
	eq(d.C4.deviceCalls[1].command, 'ROOM_OFF')
end)

test('nothing is published while disconnected', function()
	local d = boot()
	d.env.ReceivedFromProxy(301, 'CLOSE', {})
	eq(#d.C4.net.sent, 0)
end)

test('OnDriverDestroyed sends DISCONNECT and closes', function()
	local d = boot()
	connect(d)
	d.env.OnDriverDestroyed()
	local pk = sent_packets(d.C4)
	eq(pk[#pk].type, 14)
	local n = #d.C4.net.connects
	d.C4:Advance(120000)
	eq(#d.C4.net.connects, n, 'no reconnect after destroy')
end)

io.write(string.format('\n%d passed, %d failed\n', passed, failed))
os.exit(failed == 0 and 0 or 1)
