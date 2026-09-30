-- DriverWorks entry points.

local function NormalizeCommand(value)
	value = string.upper(U.Trim(value))
	value = string.gsub(value, '[%s%-/]+', '_')
	return value
end

local function Param(tParams, ...)
	for _, key in ipairs({...}) do
		local v = tParams[key]
		if (v ~= nil and v ~= '') then
			return v
		end
	end
	return nil
end

function OnDriverInit(driverInitType)
	STATE.destroying = false
	STATE.initDone = false
	if (C4 and C4.AddVariable) then
		U.SafeCall(function()
			C4:AddVariable('CONNECTED', '0', 'BOOL', true, false)
			C4:AddVariable('LAST_REMOTE_CODE', '', 'STRING', true, false)
			C4:AddVariable('LAST_REMOTE_BUTTON', '', 'STRING', true, false)
			C4:AddVariable('LAST_REMOTE_NAME', '', 'STRING', true, false)
			C4:AddVariable('SERIAL_1_RX', '', 'STRING', true, false)
			C4:AddVariable('SERIAL_2_RX', '', 'STRING', true, false)
		end)
	end
	REMOTES.Load()
	DISCO.Load()
	-- AddDynamicBinding is safe in OnDriverInit (DriverWorks "Safe Usage"
	-- table); doing it here lets Director restore the Composer connections.
	IO.RestoreIrBindings()
end

function OnDriverLateInit(driverInitType)
	U.UpdateProperty('Driver Version', DRIVER_VERSION)
	PROPS.ApplyAll()
	IO.ApplyIrBindings()
	CLIENT.onUp = IO.OnUp
	CLIENT.onDown = IO.OnDown
	CLIENT.onMessage = IO.OnMessage
	REMOTES.RegisterAllEvents()
	REMOTES.Disarm('Idle')
	STATE.initDone = true
	DISCO.onChange = function()
		if (not PROPS.IsBroker()) then PROPS.ApplyConnection() end
	end
	PROPS.ApplyMode()
	STATE.tickTimer = U.CancelTimer(STATE.tickTimer)
	STATE.tickTimer = U.SetTimer(1000, function()
		CLIENT.Tick()
		REMOTES.Tick()
		DISCO.Tick()
	end, true)
end

function OnDriverDestroyed(driverInitType)
	STATE.destroying = true
	STATE.tickTimer = U.CancelTimer(STATE.tickTimer)
	CLIENT.Stop()
	DISCO.Stop()
end

function OnPropertyChanged(strProperty)
	if (STATE.destroying) then
		return
	end
	PROPS.Apply(strProperty)
end

function ReceivedFromProxy(idBinding, strCommand, tParams)
	if (STATE.destroying) then
		return
	end
	if (not IO.FromProxy(tonumber(idBinding), strCommand, tParams or {})) then
		U.Debug('ReceivedFromProxy ' .. tostring(idBinding) .. ' ' .. tostring(strCommand))
	end
end

function OnBindingChanged(idBinding, strClass, bIsBound, otherDeviceID, otherBindingID)
	if (STATE.destroying) then
		return
	end
	local bound = (bIsBound == true or U.IsTrue(bIsBound))
	IO.OnBindingChanged(tonumber(idBinding), strClass, bound, otherDeviceID, otherBindingID)
end

function OnConnectionStatusChanged(idBinding, nPort, strStatus)
	if (STATE.destroying) then
		return
	end
	CLIENT.OnConnectionStatus(idBinding, nPort, strStatus)
end

function ReceivedFromNetwork(idBinding, nPort, strData)
	if (STATE.destroying) then
		return
	end
	if (tonumber(idBinding) == ZRP.MDNS_BINDING) then
		DISCO.OnPacket(strData)
		return
	end
	CLIENT.OnData(idBinding, nPort, strData)
end

-- The mDNS listener when it is a C4:CreateServer UDP socket (09_discovery.lua).
-- This driver runs no other server.
function OnServerDataIn(nHandle, strData, strAddress, nPort, strIdentifier)
	if (STATE.destroying) then
		return
	end
	if (strIdentifier == nil or strIdentifier == 'zrp6-mdns') then
		DISCO.OnPacket(strData)
	end
end

function OnServerStatusChanged(nPort, strStatus, strIdentifier)
	if (STATE.destroying) then
		return
	end
	if (tonumber(nPort) == DISCO.PORT) then
		DISCO.OnServerStatus(strStatus)
	end
end

-- Requests from the Remote Buttons tab (10_ui.lua). The JSON answer is
-- returned for a caller that reads the REST reply; the page itself listens
-- for the SendDataToUI push.
function UIRequest(strCommand, tParams)
	if (STATE.destroying) then
		return nil
	end
	local answer = UI.Request(strCommand, tParams)
	if (answer == nil) then
		U.Debug('unhandled UIRequest ' .. tostring(strCommand))
	end
	return answer
end

-- CUSTOM_SELECT for the Press Remote Button command.
function GetRemoteButtonList(currentValue)
	local list = {}
	for _, btn in ipairs(REMOTES.SortedButtons()) do
		table.insert(list, {text = REMOTES.Label(btn), value = btn.code})
	end
	return list
end

local OUTPUTS = {
	['RELAY 1'] = {'relay', 1}, ['RELAY 2'] = {'relay', 2}, ['RELAY 3'] = {'relay', 3},
	['TRIGGER 1'] = {'trigger', 1}, ['TRIGGER 2'] = {'trigger', 2}, ['TRIGGER 3'] = {'trigger', 3},
}

local COMMANDS = {}

function COMMANDS.RECONNECT()
	CLIENT.Reconnect()
end

function COMMANDS.RESEND_PORT_CONFIGURATION()
	IO.PushAll()
end

function COMMANDS.FORGET_SELECTED_BOX()
	DISCO.Clear()
end

function COMMANDS.SET_OUTPUT(tParams)
	local out = OUTPUTS[string.upper(U.Trim(Param(tParams, 'Output', 'OUTPUT') or ''))]
	local state = string.upper(U.Trim(Param(tParams, 'State', 'STATE') or ''))
	if (not out) then
		U.SetLastError('Set Output: unknown output')
		return
	end
	if (state == 'ON') then
		IO.SetOutput(out[1], out[2], true)
	elseif (state == 'OFF') then
		IO.SetOutput(out[1], out[2], false)
	elseif (state == 'TOGGLE') then
		IO.ToggleOutput(out[1], out[2])
	elseif (state == 'PULSE') then
		IO.PulseOutput(out[1], out[2], Param(tParams, 'Time (ms)', 'TIME'))
	end
end

function COMMANDS.SEND_IR(tParams)
	local port = tonumber(Param(tParams, 'Port', 'PORT'))
	local code = Param(tParams, 'Code', 'CODE')
	if (not port or port < 1 or port > ZRP.IR_PORTS or not code) then
		U.SetLastError('Send IR: need a port 1-' .. ZRP.IR_PORTS .. ' and a code')
		return
	end
	IO.SendIR(port, code, tonumber(Param(tParams, 'Repeats', 'REPEATS')))
end

function COMMANDS.SEND_SERIAL(tParams)
	local port = string.upper(U.Trim(Param(tParams, 'Port', 'PORT') or ''))
	local data = U.DecodeEscapes(Param(tParams, 'Data', 'DATA') or '')
	local rs, ir = tonumber(string.match(port, '^RS%-232 (%d)$')), tonumber(string.match(port, '^IR (%d)$'))
	if (rs and rs <= ZRP.SERIAL_PORTS) then
		IO.SendSerial('serial', rs, data)
	elseif (ir and ir <= ZRP.IR_PORTS) then
		IO.SendSerial('irserial', ir, data)
	else
		U.SetLastError('Send Serial: unknown port ' .. port)
	end
end

function COMMANDS.PRESS_REMOTE_BUTTON(tParams)
	local code = Param(tParams, 'Button', 'BUTTON')
	if (code) then
		REMOTES.OnCode(code)
	end
end

function ExecuteCommand(strCommand, tParams)
	if (STATE.destroying) then
		return
	end
	tParams = tParams or {}
	local command = NormalizeCommand(strCommand)
	if (command == 'LUA_ACTION') then
		command = NormalizeCommand(tParams.ACTION or tParams.action or '')
	end
	local fn = COMMANDS[command]
	if (fn) then
		fn(tParams)
	else
		U.Debug('unhandled ExecuteCommand ' .. tostring(strCommand))
	end
end

-- Lets the plain-Lua test harness reach the modules. Nil on a controller.
if (ZRP6_TEST_HOOK) then
	ZRP6_TEST_HOOK({
		ZRP = ZRP, STATE = STATE, U = U, JSON = JSON, MQTT = MQTT, CLIENT = CLIENT, IO = IO,
		ROOMS = ROOMS, REMOTES = REMOTES, DISCO = DISCO, UI = UI, PROPS = PROPS, COMMANDS = COMMANDS,
	})
end
