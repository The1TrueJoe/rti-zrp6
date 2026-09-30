-- Finding the box on the network: mDNS announcements.
--
-- The box cannot spare a socket to answer mDNS queries, so it only
-- announces: at boot, every 60 s and whenever its address changes it
-- multicasts one unsolicited response to 224.0.0.251:5353 holding
--
--   PTR _zrp6._tcp.local          -> zrp6-XXXX._zrp6._tcp.local
--   SRV zrp6-XXXX._zrp6._tcp.local -> 0 0 1883 zrp6-XXXX.local
--   TXT zrp6-XXXX._zrp6._tcp.local -> "mac=025A52D17A36"
--   A   zrp6-XXXX.local            -> the box's IPv4 address
--
-- (XXXX = the last two MAC bytes). The driver listens for those, lists the
-- boxes it has heard in the "Box" property, remembers the chosen one by MAC,
-- and follows its address when a new announcement shows a different one.
--
-- Listening. DriverWorks has no documented multicast API. Two ways in, tried
-- in order (property "Discovery Listener", Auto):
--   1. C4:CreateServer(5353, '', true) - a UDP server socket on port 5353.
--      Linux delivers group traffic to every socket bound to the port once
--      any socket on the host has joined the group (IP_MULTICAST_ALL), which
--      the controller's own mDNS stack will have done - if it runs one, and if
--      Director binds with address reuse. Neither is documented.
--   2. A UDP network connection to 224.0.0.251:5353 with MIRROR_UDP_PORT
--      (source port = destination port, NetPortOptions), received through
--      ReceivedFromNetwork - the same shape as Control4's own SSDP module
--      (drivers-common-public module/ssdp.lua).
-- Any mDNS datagram at all (not only a box's) proves a listener works; with
-- nothing heard for 3 minutes, Auto moves on to the other one. If neither
-- hears anything on a given controller, type the box's address - or its
-- name, zrp6-XXXX.local - into Box IP Address.

DISCO.SERVICE = '_zrp6._tcp.local'
DISCO.PORT = 5353
DISCO.GROUP = '224.0.0.251'
DISCO.QUIET_SWITCH_MS = 180000

DISCO.boxes = {}        -- [mac] = {mac, name, host, ip, port, seen}
DISCO.chosen = nil      -- {mac, name, ip, port}, persisted
DISCO.listener = nil    -- 'server' | 'binding' | nil
DISCO.startedAt = 0
DISCO.packets = 0       -- mDNS datagrams heard (any)
DISCO.labelToMac = {}

-- DNS wire format ----------------------------------------------------------------

local function U16(data, pos)
	local hi, lo = string.byte(data, pos, pos + 1)
	if (not lo) then return nil end
	return hi * 256 + lo
end

-- Reads a possibly compressed name at pos. Returns name (as sent), next
-- position in the original stream; or nil on anything malformed.
function DISCO.ReadName(data, pos)
	local labels = {}
	local nextPos = nil
	local jumps = 0
	while (true) do
		local len = string.byte(data, pos)
		if (not len) then return nil end
		if (len == 0) then
			return table.concat(labels, '.'), nextPos or (pos + 1)
		elseif (len >= 192) then
			local lo = string.byte(data, pos + 1)
			if (not lo) then return nil end
			nextPos = nextPos or (pos + 2)
			pos = (len - 192) * 256 + lo + 1
			jumps = jumps + 1
			if (jumps > 20) then return nil end
		elseif (len >= 64) then
			return nil
		else
			if (pos + len > string.len(data) or #labels > 40) then return nil end
			table.insert(labels, string.sub(data, pos + 1, pos + len))
			pos = pos + 1 + len
		end
	end
end

local function Txt(data, pos, last)
	local out = {}
	while (pos <= last) do
		local len = string.byte(data, pos)
		local entry = string.sub(data, pos + 1, pos + len)
		local k, v = string.match(entry, '^([^=]+)=(.*)$')
		if (k) then out[string.lower(k)] = v end
		pos = pos + 1 + len
	end
	return out
end

-- Parses one mDNS datagram. Returns a list of boxes it announces:
-- {name = 'zrp6-7A36', host = 'zrp6-7A36.local', mac = '025A52D17A36', ip, port}.
-- Anything that is not a response, or not ours, gives an empty list.
function DISCO.Parse(data)
	local boxes = {}
	if (type(data) ~= 'string' or string.len(data) < 12) then return boxes end
	local flags = U16(data, 3)
	if (flags < 32768) then return boxes end -- a query, not a response
	local qd, an, ns, ar = U16(data, 5), U16(data, 7), U16(data, 9), U16(data, 11)
	local pos = 13
	for _ = 1, qd do
		local name
		name, pos = DISCO.ReadName(data, pos)
		if (not name) then return boxes end
		pos = pos + 4
	end
	local instances, srv, txt, addr = {}, {}, {}, {}
	local suffix = '.' .. DISCO.SERVICE
	for _ = 1, an + ns + ar do
		local name
		name, pos = DISCO.ReadName(data, pos)
		if (not name or pos + 9 > string.len(data)) then break end
		local rtype, ttlHi, ttlLo, rdlen = U16(data, pos), U16(data, pos + 4), U16(data, pos + 6), U16(data, pos + 8)
		local rd = pos + 10
		if (rd + rdlen - 1 > string.len(data)) then break end
		local key = string.lower(name)
		local live = (ttlHi + ttlLo) > 0 -- TTL 0 is a goodbye
		if (live and rtype == 12 and key == DISCO.SERVICE) then
			local target = DISCO.ReadName(data, rd)
			if (target) then instances[string.lower(target)] = target end
		elseif (live and rtype == 33 and rdlen >= 7) then
			srv[key] = {port = U16(data, rd + 4), target = DISCO.ReadName(data, rd + 6)}
		elseif (live and rtype == 16) then
			txt[key] = Txt(data, rd, rd + rdlen - 1)
		elseif (live and rtype == 1 and rdlen == 4) then
			addr[key] = string.format('%d.%d.%d.%d', string.byte(data, rd, rd + 3))
		end
		pos = rd + rdlen
	end
	-- An instance is anything under _zrp6._tcp.local named by a PTR, SRV or TXT.
	for key, _ in pairs(srv) do instances[key] = instances[key] or key end
	for key, _ in pairs(txt) do instances[key] = instances[key] or key end
	for key, original in pairs(instances) do
		if (string.sub(key, -string.len(suffix)) == suffix) then
			local label = string.sub(original, 1, string.len(original) - string.len(suffix))
			local s = srv[key]
			local host = (s and s.target) or (label .. '.local')
			local mac = string.upper(tostring((txt[key] or {}).mac or ''))
			local ip = addr[string.lower(host)]
			if (string.match(mac, '^%x%x%x%x%x%x%x%x%x%x%x%x$') and ip) then
				table.insert(boxes, {name = label, host = host, mac = mac, ip = ip, port = (s and s.port) or 1883})
			end
		end
	end
	return boxes
end

-- Box list and choice ------------------------------------------------------------

local function Describe(box)
	return tostring(box.name) .. ' (' .. tostring(box.mac) .. ') at ' .. tostring(box.ip)
end

function DISCO.Load()
	local chosen = U.PersistGet(ZRP.PERSIST_BOX)
	if (type(chosen) == 'table' and chosen.mac) then
		DISCO.chosen = chosen
	end
end

-- The address the driver should use: the manual Box IP Address if one is
-- typed, else the chosen box's last known address. Returns ip, port, source.
function DISCO.Target(manualIp, manualPort)
	manualIp = U.Trim(manualIp)
	if (manualIp ~= '') then
		return manualIp, manualPort, 'manual'
	end
	local c = DISCO.chosen
	if (c and c.ip and c.ip ~= '') then
		return c.ip, tonumber(c.port) or manualPort, 'discovered'
	end
	return '', manualPort, 'none'
end

function DISCO.RefreshProperties()
	local labels, selected = {}, ''
	DISCO.labelToMac = {}
	local macs = {}
	for mac, _ in pairs(DISCO.boxes) do table.insert(macs, mac) end
	local c = DISCO.chosen
	if (c and not DISCO.boxes[c.mac]) then table.insert(macs, c.mac) end
	table.sort(macs)
	for _, mac in ipairs(macs) do
		local box = DISCO.boxes[mac] or c
		local label = Describe(box)
		DISCO.labelToMac[label] = mac
		table.insert(labels, label)
		if (c and c.mac == mac) then selected = label end
	end
	if (C4 and C4.UpdatePropertyList) then
		U.SafeCall(function() C4:UpdatePropertyList('Box', table.concat(labels, ','), selected) end)
	end
	local text
	if (c) then
		local box = DISCO.boxes[c.mac]
		text = Describe(c) .. (box and (', heard ' .. math.floor((U.Now() - box.seen) / 1000) .. ' s ago') or ', not heard since the driver started')
	else
		text = 'None chosen'
	end
	U.UpdateProperty('Selected Box', text)
	local listener = DISCO.listener == 'server' and 'UDP server on port 5353'
		or DISCO.listener == 'binding' and 'UDP binding to 224.0.0.251:5353' or 'off'
	U.UpdateProperty('Discovery Status', 'Listening: ' .. listener .. '; ' .. DISCO.packets .. ' mDNS packets heard, '
		.. #macs .. ' box' .. (#macs == 1 and '' or 'es') .. ' known')
end

-- Chooses a box by MAC, remembers it, and points the connection at it.
function DISCO.Choose(mac)
	local box = DISCO.boxes[mac] or (DISCO.chosen and DISCO.chosen.mac == mac and DISCO.chosen)
	if (not box) then
		return false
	end
	DISCO.chosen = {mac = box.mac, name = box.name, ip = box.ip, port = box.port}
	U.PersistSet(ZRP.PERSIST_BOX, DISCO.chosen)
	U.Log('using box ' .. Describe(box))
	DISCO.RefreshProperties()
	if (DISCO.onChange) then U.SafeCall(DISCO.onChange) end
	return true
end

function DISCO.Clear()
	DISCO.chosen = nil
	U.PersistSet(ZRP.PERSIST_BOX, {})
	DISCO.RefreshProperties()
	if (DISCO.onChange) then U.SafeCall(DISCO.onChange) end
end

-- One datagram from whichever listener is running.
function DISCO.OnPacket(data)
	DISCO.packets = DISCO.packets + 1
	local changed = false
	for _, box in ipairs(DISCO.Parse(data)) do
		local known = DISCO.boxes[box.mac]
		box.seen = U.Now()
		if (not known or known.ip ~= box.ip or known.name ~= box.name) then
			changed = true
			U.Log('mDNS: ' .. Describe(box))
		end
		DISCO.boxes[box.mac] = box
		local c = DISCO.chosen
		if (c and c.mac == box.mac and (c.ip ~= box.ip or tonumber(c.port) ~= box.port)) then
			U.Log('box ' .. box.name .. ' moved from ' .. tostring(c.ip) .. ' to ' .. box.ip)
			DISCO.Choose(box.mac)
		elseif (not c and U.Trim(U.Prop('Box IP Address', '')) == '') then
			-- Nothing chosen and nothing typed: take the first box heard.
			DISCO.Choose(box.mac)
		end
	end
	if (changed or DISCO.packets == 1) then
		DISCO.RefreshProperties()
	end
end

-- Listeners -------------------------------------------------------------------------

local function StopListener()
	if (DISCO.listener == 'server' and C4 and C4.DestroyServer) then
		U.SafeCall(function() C4:DestroyServer(DISCO.PORT) end)
	elseif (DISCO.listener == 'binding' and C4 and C4.NetDisconnect) then
		U.SafeCall(function() C4:NetDisconnect(ZRP.MDNS_BINDING, DISCO.PORT) end)
	end
	DISCO.listener = nil
end

local function StartServer()
	if (not (C4 and C4.CreateServer)) then return false end
	local ok, err = U.SafeCall(function() return C4:CreateServer(DISCO.PORT, '', true, 'zrp6-mdns') end)
	if (ok == false or ok == nil) then
		U.Log('mDNS: UDP server on 5353 refused (' .. tostring(err) .. ')')
		return false
	end
	DISCO.listener = 'server'
	return true
end

local function StartBinding()
	if (not (C4 and C4.CreateNetworkConnection)) then return false end
	U.SafeCall(function()
		C4:CreateNetworkConnection(ZRP.MDNS_BINDING, DISCO.GROUP, 'UDP')
		C4:NetPortOptions(ZRP.MDNS_BINDING, DISCO.PORT, 'UDP', {
			MIRROR_UDP_PORT = true,          -- receive on 5353, where announcements go
			SUPPRESS_CONNECTION_EVENTS = true,
			AUTO_CONNECT = false,
			KEEP_CONNECTION = true,
		})
		C4:NetConnect(ZRP.MDNS_BINDING, DISCO.PORT, 'UDP')
	end)
	DISCO.listener = 'binding'
	return true
end

-- Director says the UDP server went down (or never came up).
function DISCO.OnServerStatus(status)
	if (DISCO.listener == 'server' and status == 'OFFLINE' and DISCO.mode == 'Auto') then
		U.Log('mDNS: UDP server on 5353 went offline; using the network binding')
		StopListener()
		StartBinding()
		DISCO.RefreshProperties()
	end
end

-- mode: 'Auto' | 'UDP server' | 'Network binding' | 'Off'
function DISCO.Start(mode)
	StopListener()
	DISCO.mode = mode or 'Auto'
	DISCO.startedAt = U.Now()
	DISCO.packetsAtStart = DISCO.packets
	if (DISCO.mode == 'Off') then
		-- nothing
	elseif (DISCO.mode == 'Network binding') then
		StartBinding()
	elseif (not StartServer() and DISCO.mode == 'Auto') then
		StartBinding()
	end
	DISCO.RefreshProperties()
end

function DISCO.Stop()
	StopListener()
end

-- Once a second: in Auto, a listener that has heard nothing at all for
-- three minutes is swapped for the other kind.
function DISCO.Tick()
	if (DISCO.mode ~= 'Auto' or not DISCO.listener) then return end
	if (DISCO.packets == DISCO.packetsAtStart and U.Now() - DISCO.startedAt > DISCO.QUIET_SWITCH_MS) then
		local nxt = (DISCO.listener == 'server') and 'binding' or 'server'
		U.Log('mDNS: nothing heard on the ' .. DISCO.listener .. ' listener; trying the ' .. nxt .. ' one')
		StopListener()
		if (nxt == 'binding') then StartBinding() else StartServer() end
		DISCO.startedAt = U.Now()
		DISCO.RefreshProperties()
	end
end
