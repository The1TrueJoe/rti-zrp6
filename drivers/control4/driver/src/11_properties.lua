-- Properties: connection settings, port modes and discovery. Remote buttons
-- are managed only on the Remote Buttons tab (10_ui.lua).

-- Connection Mode: 'Direct to box (P2P)' talks to the box's own broker
-- (found by mDNS or typed in); 'Central broker' talks to a house broker that
-- bridges to the box. Everything past the connection is the same.
PROPS.P2P_PROPS = {'Box', 'Selected Box', 'Box IP Address', 'Box Address In Use', 'Discovery Listener',
	'Discovery Status', 'MQTT Port', 'Username', 'Password', 'Base Topic'}
PROPS.BROKER_PROPS = {'Broker Host', 'Broker Port', 'Broker Username', 'Broker Password', 'Broker Base Topic'}

function PROPS.IsBroker()
	return string.find(string.lower(tostring(U.Prop('Connection Mode', ''))), 'broker', 1, true) ~= nil
end

local function Base(value)
	local base = string.gsub(U.Trim(value), '/+$', '')
	if (base == '') then base = 'zrp6' end
	return base
end

function PROPS.ShowModeProperties()
	local broker = PROPS.IsBroker()
	for _, name in ipairs(PROPS.P2P_PROPS) do U.PropertyVisible(name, not broker) end
	for _, name in ipairs(PROPS.BROKER_PROPS) do U.PropertyVisible(name, broker) end
end

function PROPS.ApplyConnection()
	local keepalive = tonumber(U.Prop('Keepalive (s)', 30)) or 30
	if (keepalive < 10) then keepalive = 10 end
	if (keepalive > 60) then keepalive = 60 end -- the box drops a client silent for 90 s
	local cfg = {keepalive = keepalive}
	if (PROPS.IsBroker()) then
		cfg.ip = U.Trim(U.Prop('Broker Host', ''))
		cfg.port = tonumber(U.Prop('Broker Port', 1883)) or 1883
		cfg.username = tostring(U.Prop('Broker Username', '') or '')
		cfg.password = tostring(U.Prop('Broker Password', '') or '')
		cfg.base = Base(U.Prop('Broker Base Topic', 'zrp6'))
		cfg.idleText = 'Not configured: set Broker Host'
	else
		local ip, port, source = DISCO.Target(U.Prop('Box IP Address', ''), tonumber(U.Prop('MQTT Port', 1883)) or 1883)
		U.UpdateProperty('Box Address In Use', ip == '' and 'none yet'
			or (ip .. ':' .. port .. (source == 'manual' and ' (Box IP Address)' or ' (discovered)')))
		cfg.ip, cfg.port = ip, port
		cfg.username = tostring(U.Prop('Username', 'admin') or '')
		cfg.password = tostring(U.Prop('Password', 'admin') or '')
		cfg.base = Base(U.Prop('Base Topic', 'zrp6'))
		cfg.idleText = 'Waiting for a box: none discovered yet, and Box IP Address is empty'
	end
	CLIENT.Configure(cfg)
end

-- Starts or stops mDNS listening to match the mode, and applies the connection.
function PROPS.ApplyMode()
	PROPS.ShowModeProperties()
	if (PROPS.IsBroker()) then
		DISCO.Stop()
	else
		DISCO.Start(U.Prop('Discovery Listener', 'Auto'))
	end
	PROPS.ApplyConnection()
end

-- Dispatcher -------------------------------------------------------------------------

function PROPS.Apply(name)
	local n = tonumber(string.match(name, '^IR (%d) '))
	local s = tonumber(string.match(name, '^RS%-232 (%d) '))
	if (name == 'Box IP Address' or name == 'MQTT Port' or name == 'Username' or name == 'Password'
		or name == 'Base Topic' or name == 'Keepalive (s)' or string.match(name, '^Broker ')) then
		if (STATE.initDone) then
			PROPS.ApplyConnection()
		end
	elseif (name == 'Connection Mode') then
		if (STATE.initDone) then
			PROPS.ApplyMode()
		end
	elseif (name == 'Box') then
		local mac = DISCO.labelToMac[U.Prop('Box', '')]
		if (mac) then
			DISCO.Choose(mac)
		end
	elseif (name == 'Discovery Listener') then
		if (STATE.initDone and not PROPS.IsBroker()) then
			DISCO.Start(U.Prop('Discovery Listener', 'Auto'))
		end
	elseif (name == 'Debug Mode') then
		STATE.debug = (U.Prop('Debug Mode', 'Off') == 'On')
	elseif ((n and n >= 1 and n <= ZRP.IR_PORTS) or (s and s >= 1 and s <= ZRP.SERIAL_PORTS)) and IO.IsEcho(name) then
		-- The driver changed this property itself to match the box: not an edit.
		return
	elseif (n and n >= 1 and n <= ZRP.IR_PORTS) then
		-- An installer's edit: the one time a port setting goes to the box.
		IO.ReadIrProperties(n)
		if (STATE.initDone) then
			IO.ApplyIrBindings()
			IO.PushIr(n)
		end
	elseif (s and s >= 1 and s <= ZRP.SERIAL_PORTS) then
		IO.ReadSerialProperties(s)
		if (STATE.initDone) then
			IO.PushSerial(s)
		end
	end
end

function PROPS.ApplyAll()
	for n = 1, ZRP.IR_PORTS do
		IO.ReadIrProperties(n)
	end
	for n = 1, ZRP.SERIAL_PORTS do
		IO.ReadSerialProperties(n)
	end
	STATE.debug = (U.Prop('Debug Mode', 'Off') == 'On')
end
