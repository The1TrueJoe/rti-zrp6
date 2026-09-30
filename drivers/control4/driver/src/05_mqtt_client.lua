-- The one MQTT connection to the box.
--
-- The box has four TCP sockets in total and never drops an MQTT client to
-- make room, so this driver holds exactly one connection: a new attempt is
-- only made from the WAIT state, after the previous socket has been torn
-- down. States:
--
--   IDLE     no box address configured
--   WAIT     disconnected; a retry timer is pending
--   TCP      NetConnect issued, waiting for ONLINE
--   MQTT     TCP up, CONNECT sent, waiting for CONNACK
--   UP       logged in and subscribed
--   STOPPED  driver shutting down
--
-- Uses the DriverWorks dynamic network connection APIs
-- (CreateNetworkConnection / NetPortOptions / NetConnect / SendToNetwork /
-- NetDisconnect) with OnConnectionStatusChanged and ReceivedFromNetwork
-- routed here from 12_callbacks.lua.

local CLIENT_DEFAULTS = {
	state = 'IDLE',
	cfg = {ip = '', port = 1883, username = 'admin', password = 'admin', base = 'zrp6', keepalive = 30},
	parser = MQTT.NewParser(),
	queue = {},
	lastTx = 0,
	lastHold = 0,
	lastRx = 0,
	stateSince = 0,
	attempt = 0,
	packetId = 0,
	retryTimer = nil,
	flushTimer = nil,
	settleTimer = nil,
	connectedSince = nil,
	-- Hooks set by the I/O layer.
	onUp = nil,       -- function()
	onSettled = nil,  -- function() after retained state has had time to arrive
	onDown = nil,     -- function(reason)
	onMessage = nil,  -- function(subTopic, payload, retain)
}
for k, v in pairs(CLIENT_DEFAULTS) do
	CLIENT[k] = v
end

local function SetStatus(text)
	CLIENT.statusText = text
	U.UpdateProperty('Connection Status', text)
end

function CLIENT.IsUp()
	return CLIENT.state == 'UP'
end

function CLIENT.Topic(sub)
	return CLIENT.cfg.base .. '/' .. sub
end

local function Raw(packet)
	if (C4 and C4.SendToNetwork) then
		U.SafeCall(function() C4:SendToNetwork(ZRP.NET_BINDING, CLIENT.cfg.port, packet) end)
	end
	CLIENT.lastTx = U.Now()
end

local function NetDisconnect()
	if (C4 and C4.NetDisconnect) then
		U.SafeCall(function() C4:NetDisconnect(ZRP.NET_BINDING, CLIENT.cfg.port) end)
	end
end

local function ClearQueue()
	CLIENT.queue = {}
	CLIENT.lastHold = 0
	CLIENT.flushTimer = U.CancelTimer(CLIENT.flushTimer)
end

-- Sends queued packets one at a time, SEND_GAP_MS apart. The box reads one
-- 768-byte buffer per poll and drops a packet that straddles the end of it,
-- so two long Pronto codes must never arrive in the same read. A packet can
-- also carry a hold time: the box is busy for the length of an IR burst or a
-- serial transmission, and packets sent meanwhile pile up in its receive
-- buffer, so the next one waits that long as well.
function CLIENT.Flush()
	if (CLIENT.flushTimer) then
		return
	end
	while (#CLIENT.queue > 0) do
		if (CLIENT.state ~= 'UP' and CLIENT.state ~= 'MQTT') then
			ClearQueue()
			return
		end
		local wait = ZRP.SEND_GAP_MS + (CLIENT.lastHold or 0) - (U.Now() - CLIENT.lastTx)
		if (wait > 0) then
			CLIENT.flushTimer = U.SetTimer(wait, function()
				CLIENT.flushTimer = nil
				CLIENT.Flush()
			end)
			if (CLIENT.flushTimer) then
				return
			end
			-- No timer API (should not happen on a controller): send anyway.
		end
		local item = table.remove(CLIENT.queue, 1)
		Raw(item.data)
		CLIENT.lastHold = item.hold or 0
	end
end

local function Enqueue(packet, holdMs)
	if (#CLIENT.queue >= 200) then
		table.remove(CLIENT.queue, 1)
		U.Log('send queue full; dropped the oldest packet')
	end
	table.insert(CLIENT.queue, {data = packet, hold = holdMs})
	CLIENT.Flush()
end

local function ScheduleRetry(delayMs, reason)
	CLIENT.retryTimer = U.CancelTimer(CLIENT.retryTimer)
	if (CLIENT.state == 'STOPPED' or CLIENT.state == 'IDLE') then
		return
	end
	SetStatus('Disconnected (' .. tostring(reason) .. '); retrying in ' .. math.floor(delayMs / 1000) .. ' s')
	CLIENT.retryTimer = U.SetTimer(delayMs, function()
		CLIENT.retryTimer = nil
		if (CLIENT.state == 'WAIT') then
			CLIENT.Connect()
		end
	end)
end

local function NextBackoffMs()
	local list = ZRP.BACKOFF_S
	local idx = CLIENT.attempt + 1
	if (idx > #list) then idx = #list end
	CLIENT.attempt = CLIENT.attempt + 1
	return list[idx] * 1000
end

-- Tears the connection down and schedules the next attempt.
function CLIENT.Drop(reason, delayMs)
	local wasUp = (CLIENT.state == 'UP')
	if (CLIENT.state == 'STOPPED' or CLIENT.state == 'IDLE') then
		return
	end
	U.Log('connection dropped: ' .. tostring(reason))
	CLIENT.state = 'WAIT'
	CLIENT.settleTimer = U.CancelTimer(CLIENT.settleTimer)
	ClearQueue()
	CLIENT.parser:Reset()
	NetDisconnect()
	CLIENT.connectedSince = nil
	if (wasUp and CLIENT.onDown) then
		U.SafeCall(CLIENT.onDown, reason)
	end
	ScheduleRetry(delayMs or NextBackoffMs(), reason)
end

function CLIENT.Connect()
	local cfg = CLIENT.cfg
	if (cfg.ip == '') then
		CLIENT.state = 'IDLE'
		SetStatus(cfg.idleText or 'Not configured')
		return
	end
	CLIENT.retryTimer = U.CancelTimer(CLIENT.retryTimer)
	ClearQueue()
	CLIENT.parser:Reset()
	U.Debug('connecting to ' .. cfg.ip .. ':' .. cfg.port)
	if (C4 and C4.CreateNetworkConnection) then
		U.SafeCall(function() C4:CreateNetworkConnection(ZRP.NET_BINDING, cfg.ip) end)
	end
	if (C4 and C4.NetPortOptions) then
		-- We own reconnects (with backoff); Director must not retry on its own
		-- or it could stack a second socket on the box.
		U.SafeCall(function()
			C4:NetPortOptions(ZRP.NET_BINDING, cfg.port, 'TCP', {
				AUTO_CONNECT = false,
				MONITOR_CONNECTION = false,
				KEEP_CONNECTION = false,
				KEEP_ALIVE = true,
			})
		end)
	end
	CLIENT.state = 'TCP'
	CLIENT.stateSince = U.Now()
	SetStatus('Connecting to ' .. cfg.ip .. ':' .. cfg.port)
	if (C4 and C4.NetConnect) then
		U.SafeCall(function() C4:NetConnect(ZRP.NET_BINDING, cfg.port) end)
	end
end

-- cfg: {ip, port, username, password, base, keepalive}. Reconnects when
-- anything that matters has changed.
function CLIENT.Configure(cfg)
	local old = CLIENT.cfg
	local changed = (old.ip ~= cfg.ip or old.port ~= cfg.port or old.idleText ~= cfg.idleText or old.username ~= cfg.username
		or old.password ~= cfg.password or old.base ~= cfg.base or old.keepalive ~= cfg.keepalive)
	if (not changed and CLIENT.state ~= 'IDLE') then
		return
	end
	-- Close the old socket on the old port before switching.
	if (CLIENT.state == 'UP') then
		Raw(MQTT.Disconnect())
	end
	if (CLIENT.state ~= 'IDLE' and CLIENT.state ~= 'STOPPED') then
		NetDisconnect()
	end
	local wasUp = (CLIENT.state == 'UP')
	CLIENT.cfg = cfg
	CLIENT.attempt = 0
	CLIENT.state = 'WAIT'
	CLIENT.settleTimer = U.CancelTimer(CLIENT.settleTimer)
	if (wasUp and CLIENT.onDown) then
		U.SafeCall(CLIENT.onDown, 'reconfigured')
	end
	if (string.len(cfg.base .. '/serial/1/config/set') > ZRP.MAX_TOPIC) then
		U.SetLastError('Base Topic "' .. cfg.base .. '" is too long: the box keeps topics of at most '
			.. ZRP.MAX_TOPIC .. ' characters, so the longest ones would be cut off. Use '
			.. (ZRP.MAX_TOPIC - string.len('/serial/1/config/set')) .. ' characters or fewer.')
	end
	if (cfg.ip == '') then
		CLIENT.state = 'IDLE'
		SetStatus(cfg.idleText or 'Not configured')
		return
	end
	-- Give a socket we just closed a moment to go before opening the next.
	ScheduleRetry(changed and 500 or 100, 'configuring')
	SetStatus('Connecting to ' .. cfg.ip .. ':' .. cfg.port)
end

function CLIENT.Reconnect()
	if (CLIENT.state == 'UP') then
		Raw(MQTT.Disconnect())
	end
	CLIENT.attempt = 0
	CLIENT.Drop('reconnect requested', 500)
end

function CLIENT.Stop()
	CLIENT.retryTimer = U.CancelTimer(CLIENT.retryTimer)
	CLIENT.settleTimer = U.CancelTimer(CLIENT.settleTimer)
	ClearQueue()
	if (CLIENT.state == 'UP') then
		Raw(MQTT.Disconnect())
	end
	if (CLIENT.state ~= 'IDLE') then
		NetDisconnect()
	end
	CLIENT.state = 'STOPPED'
end

local function NextPacketId()
	CLIENT.packetId = (CLIENT.packetId % 65535) + 1
	return CLIENT.packetId
end

-- A client ID starting with '!' is a priority client on the box: never
-- bumped to free a socket. Used in both modes (a broker passes it through
-- only for its own bridge, which should use a '!' ID as well - see README).
function CLIENT.ClientId()
	local id = U.DeviceId()
	return '!c4-zrp6-' .. tostring(id or math.random(1000, 9999))
end

-- Director callbacks ---------------------------------------------------------

function CLIENT.OnConnectionStatus(idBinding, nPort, strStatus)
	if (tonumber(idBinding) ~= ZRP.NET_BINDING) then
		return
	end
	U.Debug('network ' .. tostring(strStatus) .. ' (state ' .. CLIENT.state .. ')')
	if (strStatus == 'ONLINE') then
		if (CLIENT.state == 'TCP') then
			CLIENT.state = 'MQTT'
			CLIENT.stateSince = U.Now()
			CLIENT.parser:Reset()
			SetStatus('Logging in')
			Raw(MQTT.Connect({
				clientId = CLIENT.ClientId(),
				username = CLIENT.cfg.username,
				password = CLIENT.cfg.password,
				keepalive = CLIENT.cfg.keepalive,
				cleanSession = true,
			}))
		elseif (CLIENT.state ~= 'MQTT' and CLIENT.state ~= 'UP') then
			-- A socket we did not ask for (late ONLINE after we gave up):
			-- close it so we never hold two.
			NetDisconnect()
		end
	elseif (strStatus == 'OFFLINE') then
		if (CLIENT.state == 'TCP' or CLIENT.state == 'MQTT' or CLIENT.state == 'UP') then
			CLIENT.Drop('connection closed')
		end
	end
end

local function HandlePacket(pkt)
	if (pkt.type == MQTT.CONNACK) then
		if (CLIENT.state ~= 'MQTT') then
			return
		end
		local _, rc = MQTT.DecodeConnack(pkt.body)
		if (rc == 0) then
			CLIENT.state = 'UP'
			CLIENT.attempt = 0
			CLIENT.connectedSince = U.Now()
			CLIENT.lastRx = U.Now()
			SetStatus('Connected to ' .. CLIENT.cfg.ip .. ':' .. CLIENT.cfg.port)
			U.Log('connected')
			Enqueue(MQTT.Subscribe(NextPacketId(), {CLIENT.Topic('#')}))
			if (CLIENT.onUp) then
				U.SafeCall(CLIENT.onUp)
			end
			CLIENT.settleTimer = U.CancelTimer(CLIENT.settleTimer)
			CLIENT.settleTimer = U.SetTimer(ZRP.SETTLE_MS, function()
				CLIENT.settleTimer = nil
				if (CLIENT.state == 'UP' and CLIENT.onSettled) then
					U.SafeCall(CLIENT.onSettled)
				end
			end)
		else
			local text = MQTT.CONNACK_TEXT[rc or -1] or ('return code ' .. tostring(rc))
			U.SetLastError('Login refused by the box: ' .. text)
			local delay = nil
			if (rc == 4 or rc == 5) then
				-- Every failed login locks the box for 2 s for everyone;
				-- don't hammer it with a wrong password.
				delay = ZRP.AUTH_BACKOFF_S * 1000
			end
			CLIENT.Drop('login refused: ' .. text, delay)
		end
	elseif (pkt.type == MQTT.SUBACK) then
		local codes = string.sub(pkt.body, 3)
		for i = 1, string.len(codes) do
			if (string.byte(codes, i) == 128) then
				U.SetLastError('The box refused the subscription to ' .. CLIENT.Topic('#'))
			end
		end
	elseif (pkt.type == MQTT.PUBLISH) then
		local msg, err = MQTT.DecodePublish(pkt.flags, pkt.body)
		if (not msg) then
			U.Log('bad PUBLISH: ' .. tostring(err))
			return
		end
		if (msg.qos == 1 and msg.packetId) then
			Enqueue(MQTT.PubAck(msg.packetId))
		end
		local prefix = CLIENT.cfg.base .. '/'
		if (string.sub(msg.topic, 1, string.len(prefix)) == prefix and CLIENT.onMessage) then
			local sub = string.sub(msg.topic, string.len(prefix) + 1)
			U.Debug('<- ' .. msg.topic .. ' ' .. U.Printable(msg.payload, 80) .. (msg.retain and ' (retained)' or ''))
			U.SafeCall(CLIENT.onMessage, sub, msg.payload, msg.retain)
		end
	elseif (pkt.type == MQTT.PINGRESP) then
		U.Debug('<- PINGRESP')
	end
end

function CLIENT.OnData(idBinding, nPort, data)
	if (tonumber(idBinding) ~= ZRP.NET_BINDING) then
		return
	end
	if (CLIENT.state ~= 'MQTT' and CLIENT.state ~= 'UP') then
		return
	end
	CLIENT.lastRx = U.Now()
	local packets, err = CLIENT.parser:Feed(data)
	if (not packets) then
		CLIENT.Drop('protocol error: ' .. tostring(err))
		return
	end
	for _, pkt in ipairs(packets) do
		HandlePacket(pkt)
		if (CLIENT.state ~= 'MQTT' and CLIENT.state ~= 'UP') then
			return
		end
	end
end

-- Called once a second.
function CLIENT.Tick()
	local now = U.Now()
	if (CLIENT.state == 'TCP' and now - CLIENT.stateSince > ZRP.CONNECT_TIMEOUT_MS) then
		CLIENT.Drop('no answer from ' .. CLIENT.cfg.ip .. ':' .. CLIENT.cfg.port)
	elseif (CLIENT.state == 'MQTT' and now - CLIENT.stateSince > ZRP.CONNACK_TIMEOUT_MS) then
		CLIENT.Drop('no CONNACK')
	elseif (CLIENT.state == 'UP') then
		local ka = CLIENT.cfg.keepalive * 1000
		if (now - CLIENT.lastRx > ka * 1.5) then
			CLIENT.Drop('keepalive timeout')
		elseif (now - CLIENT.lastTx >= ka / 2) then
			Enqueue(MQTT.PingReq())
		end
	end
end

-- Publishes base/sub at QoS 0. holdMs: how long the box will be busy acting
-- on it (see Flush). Returns true if queued.
function CLIENT.Publish(sub, payload, retain, holdMs)
	local topic = CLIENT.Topic(sub)
	if (CLIENT.state ~= 'UP') then
		U.Log('not connected; dropped ' .. topic .. ' ' .. U.Printable(payload, 40))
		return false
	end
	if (string.len(topic) > ZRP.MAX_TOPIC) then
		U.SetLastError('Topic too long for the box (' .. string.len(topic) .. ' > ' .. ZRP.MAX_TOPIC .. '): ' .. topic)
		return false
	end
	local packet = MQTT.Publish(topic, payload, retain)
	if (string.len(packet) > ZRP.MAX_PACKET) then
		U.SetLastError('Message too long for the box (' .. string.len(packet) .. ' bytes): ' .. topic)
		return false
	end
	U.Debug('-> ' .. topic .. ' ' .. U.Printable(payload, 80))
	Enqueue(packet, holdMs)
	return true
end
