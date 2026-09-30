-- Lua side of the mosquitto integration test (run_integration.py drives it).
--
-- Runs the real driver.lua against the mock C4, except that the network
-- binding is a real TCP socket owned by the Python side. Line protocol on
-- stdin/stdout, all binary data hex-encoded:
--
--   in:  TICK <ms> | ONLINE | OFFLINE | DATA <hex> | EXEC <hex lua>
--   out: CONNECT <host> <port> | SEND <hex> | CLOSE | RESULT <hex> | END
--
-- Every input line is answered with any requests it caused, then END.

local here = (arg and arg[0] or ''):match('^(.*)[/\\]') or '.'
package.path = here .. '/../?.lua;' .. package.path
local Mock = require('mock_c4')

local DRIVER = arg[1]
local PROPS = {}
for i = 2, #arg do
	local k, v = arg[i]:match('^([^=]+)=(.*)$')
	if (k) then PROPS[k] = v end
end

local function tohex(s) return (s:gsub('.', function(c) return string.format('%02x', c:byte()) end)) end
local function fromhex(h) return (h:gsub('%x%x', function(x) return string.char(tonumber(x, 16)) end)) end

local out = {}
local function emit(line) out[#out + 1] = line end

local C4 = Mock.new({
	-- C4:GetDevices shape (DriverWorks API reference, GetDevices Example 1); rooms
	-- 14 and 31 match test/fixtures/director/rooms.json.
	devices = {
		[27] = {driverFileName = 'control4_sr150.c4i', deviceName = 'SR-150', roomId = '10', roomName = 'Den'},
		[57] = {driverFileName = 'control4_sr260.c4i', deviceName = 'System Remote Control SR260', roomId = '31', roomName = 'Bathroom'},
		[36] = {driverFileName = 'rf_cable.c4i', deviceName = 'Cable TV', roomId = '14', roomName = 'Room'},
	},
	sources = {[10] = {watch = {77}, listen = {}}, [14] = {watch = {36}, listen = {100002}}, [31] = {watch = {50}, listen = {}}},
})
local address = '127.0.0.1'
-- Only binding 6001 (the MQTT connection) is bridged to a real socket.
function C4:CreateNetworkConnection(binding, addr) if (binding == 6001) then address = addr end end
function C4:NetPortOptions() end
function C4:NetConnect(binding, port) if (binding == 6001) then emit('CONNECT ' .. address .. ' ' .. port) end end
function C4:NetDisconnect(binding, port) if (binding == 6001) then emit('CLOSE') end end
function C4:SendToNetwork(binding, port, data) if (binding == 6001) then emit('SEND ' .. tohex(data)) end end

local props = {
	['Box IP Address'] = '127.0.0.1', ['MQTT Port'] = '1883', ['Username'] = 'admin', ['Password'] = 'admin',
	['Base Topic'] = 'zrp6', ['Keepalive (s)'] = '30', ['Debug Mode'] = 'Off', ['Relay Pulse Time (ms)'] = '500',
	['Remote Mapper Port'] = '8765', ['Remote Mapper Password'] = '',
}
for n = 1, 7 do
	props['IR ' .. n .. ' Mode'] = 'IR'
	props['IR ' .. n .. ' Serial Baud'] = 'Auto (bound device)'
	props['IR ' .. n .. ' Serial Polarity'] = 'RS-232 (idle low)'
end
for n = 1, 2 do
	props['RS-232 ' .. n .. ' Baud'] = 'Auto (bound device)'
	props['RS-232 ' .. n .. ' Format'] = 'Auto (bound device)'
end
for k, v in pairs(PROPS) do props[k] = v end

local env, m = Mock.load(DRIVER, C4, props)
local d = {C4 = C4, env = env, m = m, props = props}
env.OnDriverInit(0)
env.OnDriverLateInit(0)

local function flush()
	for _, line in ipairs(out) do io.write(line, '\n') end
	io.write('END\n')
	io.flush()
	out = {}
end
flush()

local sandbox = setmetatable({d = d, C4 = C4, env = env, m = m, fromhex = fromhex, tohex = tohex}, {__index = _G})

for line in io.lines() do
	local cmd, rest = line:match('^(%S+)%s*(.*)$')
	if (cmd == 'TICK') then
		C4:Advance(tonumber(rest))
	elseif (cmd == 'ONLINE' or cmd == 'OFFLINE') then
		env.OnConnectionStatusChanged(6001, tonumber(props['MQTT Port']), cmd)
	elseif (cmd == 'DATA') then
		env.ReceivedFromNetwork(6001, tonumber(props['MQTT Port']), fromhex(rest))
	elseif (cmd == 'EXEC') then
		local code = fromhex(rest)
		local fn, err
		if (setfenv) then
			fn, err = loadstring(code, '=exec')
			if (fn) then setfenv(fn, sandbox) end
		else
			fn, err = load(code, '=exec', 't', sandbox)
		end
		local ok, result = false, err
		if (fn) then ok, result = pcall(fn) end
		emit('RESULT ' .. tohex((ok and '' or 'ERROR: ') .. tostring(result)))
	elseif (cmd == 'QUIT') then
		flush()
		break
	end
	flush()
end
