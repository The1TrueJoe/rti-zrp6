-- Rooms, and running the room actions a remote button is mapped to.
--
-- The Remote Buttons tab gets the rooms and each room's commands from
-- Director itself (GET /api/v1/rooms, GET /api/v1/items/<room>/commands),
-- so an action is whatever Control4 lists for that room, stored as
--
--   {kind = 'command', room = 14, command = 'ROOM_OFF', params = {...},
--    release = 'STOP_VOL_UP' (optional), label = 'Room Off'}
--   {kind = 'source',  room = 14, source = 77, label = 'Apple TV'}
--   {kind = 'event'}                     (fire the programming event only)
--
-- and replayed with C4:SendToDevice(room, command, params) - the same call
-- The1TrueJoe/control4-roomcontrol-webhook uses to drive rooms. A remote
-- press has no release, so a START_x command is always followed by its
-- STOP_x (a tap), as that driver does.
--
-- The Lua room/source lookups below are the fallback for when the tab cannot
-- reach Director's REST API.

ROOMS.cache = nil

-- 1.0.0 stored fixed action ids; they still run.
local LEGACY = {
	ROOM_OFF = {'ROOM_OFF'}, VOLUME_UP = {'START_VOL_UP', 'STOP_VOL_UP'}, VOLUME_DOWN = {'START_VOL_DOWN', 'STOP_VOL_DOWN'},
	MUTE_TOGGLE = {'MUTE_TOGGLE'}, MUTE_ON = {'MUTE_ON'}, MUTE_OFF = {'MUTE_OFF'}, PLAYPAUSE = {'PLAYPAUSE'},
	PLAY = {'PLAY'}, PAUSE = {'PAUSE'}, STOP = {'STOP'}, SKIP_FWD = {'SKIP_FWD'}, SKIP_REV = {'SKIP_REV'},
	SCAN_FWD = {'START_SCAN_FWD', 'STOP_SCAN_FWD'}, SCAN_REV = {'START_SCAN_REV', 'STOP_SCAN_REV'},
	CH_UP = {'START_CH_UP', 'STOP_CH_UP'}, CH_DOWN = {'START_CH_DOWN', 'STOP_CH_DOWN'}, UP = {'UP'}, DOWN = {'DOWN'},
	LEFT = {'LEFT'}, RIGHT = {'RIGHT'}, ENTER = {'ENTER'}, MENU = {'MENU'}, GUIDE = {'GUIDE'}, INFO = {'INFO'},
	CANCEL = {'CANCEL'}, BACK = {'BACK'}, EXIT = {'EXIT'}, RECALL = {'RECALL'},
	PAGE_UP = {'START_PAGE_UP', 'STOP_PAGE_UP'}, PAGE_DOWN = {'START_PAGE_DOWN', 'STOP_PAGE_DOWN'}, RECORD = {'RECORD'},
}
for d = 0, 9 do
	LEGACY['NUMBER_' .. d] = {'NUMBER_' .. d}
end

local function CommandName(value)
	value = string.upper(U.Trim(value))
	if (string.match(value, '^[A-Z0-9_]+$')) then
		return value
	end
	return nil
end

local function CleanParams(params)
	local out = {}
	local n = 0
	if (type(params) ~= 'table') then
		return out
	end
	for k, v in pairs(params) do
		if (type(k) == 'string' and string.match(k, '^[%w_]+$') and (type(v) == 'string' or type(v) == 'number') and n < 20) then
			out[k] = tostring(v)
			n = n + 1
		end
	end
	return out
end

-- Returns a clean action, or nil. Accepts the 1.0.0 shape too.
function ROOMS.Normalize(a)
	if (type(a) ~= 'table') then
		return nil
	end
	local room = tonumber(a.room)
	local label = string.sub(U.Trim(a.label), 1, 60)
	local roomName = string.sub(U.Trim(a.roomName), 1, 60)
	if (a.kind == nil and a.action ~= nil) then
		local id = tostring(a.action)
		if (id == 'EVENT_ONLY') then return {kind = 'event'} end
		if (id == 'SELECT_SOURCE') then return ROOMS.Normalize({kind = 'source', room = room, source = a.source}) end
		if (id == 'SET_VOLUME') then
			return ROOMS.Normalize({kind = 'command', room = room, command = 'SET_VOLUME_LEVEL', params = {LEVEL = tostring(a.level or '')}})
		end
		if (id == 'CUSTOM') then return ROOMS.Normalize({kind = 'command', room = room, command = a.command}) end
		local seq = LEGACY[id]
		if (not seq) then return nil end
		return {kind = 'command', room = room, command = seq[1], release = seq[2], params = {}, label = id}
	end
	if (a.kind == 'event') then
		return {kind = 'event'}
	elseif (a.kind == 'source') then
		local source = tonumber(a.source)
		if (not room or not source) then return nil end
		return {kind = 'source', room = room, roomName = roomName, source = source, label = label}
	elseif (a.kind == 'command' or a.kind == nil) then
		local command = CommandName(a.command)
		if (not room or not command) then return nil end
		local release = a.release and CommandName(a.release) or nil
		local x = string.match(command, '^START_(.+)$')
		if (not release and x) then
			release = 'STOP_' .. x -- a remote press has no release: tap
		end
		return {kind = 'command', room = room, roomName = roomName, command = command, params = CleanParams(a.params),
			release = release, label = label}
	end
	return nil
end

-- Room discovery (fallback) -------------------------------------------------------

function ROOMS.Name(roomId)
	local name = nil
	if (C4 and C4.ListGetDeviceName) then
		name = U.SafeCall(function() return C4:ListGetDeviceName(roomId) end)
	end
	if ((name == nil or name == '') and C4 and C4.GetDeviceDisplayName) then
		name = U.SafeCall(function() return C4:GetDeviceDisplayName(roomId) end)
	end
	if ((name == nil or name == '') and ROOMS.cache) then
		for _, r in ipairs(ROOMS.cache) do
			if (r.id == tonumber(roomId)) then name = r.name end
		end
	end
	if (name == nil or name == '') then
		name = 'Room ' .. tostring(roomId)
	end
	return tostring(name)
end

-- C4:GetDevices({}) gives every device with its roomId/roomName. On a real
-- OS 3.3.3 controller GetProjectHierarchy returned nothing usable while the
-- devices carried their rooms (junohouse/control4-adapter agent/driver.lua,
-- roomsFrom), which is why the hierarchy walk this driver used to rely on
-- left the room list empty.
local function CollectFromDevices(rooms)
	if (not (C4 and C4.GetDevices)) then return end
	local all = U.SafeCall(function() return C4:GetDevices({}) end)
	if (type(all) ~= 'table') then return end
	for id, d in pairs(all) do
		if (type(d) == 'table') then
			local roomId = tonumber(d.roomId or d.roomID)
			if (roomId and roomId > 0 and d.roomName and not rooms[roomId]) then
				rooms[roomId] = tostring(d.roomName)
			end
			if (tostring(d.driverFileName or '') == 'roomdevice.c4i' and tonumber(id) and not rooms[tonumber(id)]) then
				rooms[tonumber(id)] = tostring(d.deviceName or ROOMS.Name(id))
			end
		end
	end
end

local function CollectFromHierarchy(node, nodeId, rooms, seen)
	if (type(node) ~= 'table' or seen[node]) then return end
	seen[node] = true
	local id = tonumber(node.id or node.ID or node.locationId or nodeId)
	local kind = string.lower(tostring(node.type or node.Type or node.locationType or ''))
	if (kind == 'room' and id and not rooms[id]) then
		rooms[id] = tostring(node.name or node.Name or ROOMS.Name(id))
	end
	for key, child in pairs(node) do
		if (type(child) == 'table' and key ~= 'parent' and key ~= 'Parent') then
			CollectFromHierarchy(child, key, rooms, seen)
		end
	end
end

function ROOMS.List(refresh)
	if (ROOMS.cache and not refresh) then
		return ROOMS.cache
	end
	local rooms = {}
	CollectFromDevices(rooms)
	if (next(rooms) == nil and C4 and C4.GetProjectHierarchy) then
		CollectFromHierarchy(U.SafeCall(function() return C4:GetProjectHierarchy() end), nil, rooms, {})
	end
	local list = {}
	for id, name in pairs(rooms) do
		table.insert(list, {id = id, name = name})
	end
	table.sort(list, function(a, b) return string.lower(a.name) < string.lower(b.name) end)
	ROOMS.cache = list
	return list
end

-- A room answers GET_WATCH_DEVICES / GET_LISTEN_DEVICES as a UI request with
-- the sources in its Watch and Listen menus (junohouse FINDINGS.md section 1,
-- agent/driver.lua sourcesFor). SendToDevice is kept as a second try.
local function SourceXml(roomId, command)
	local xml = nil
	if (C4 and C4.SendUIRequest) then
		xml = U.SafeCall(function() return C4:SendUIRequest(roomId, command, {hidden = 0}) end)
	end
	if ((type(xml) ~= 'string' or xml == '') and C4 and C4.SendToDevice) then
		xml = U.SafeCall(function() return C4:SendToDevice(roomId, command, {}) end)
	end
	return type(xml) == 'string' and xml or ''
end

local function SourceIds(roomId, command)
	local ids = {}
	for fragment in string.gmatch(SourceXml(roomId, command), '<source>(.-)</source>') do
		local id = tonumber(string.match(fragment, '<id>(.-)</id>'))
		if (id) then ids[id] = true end
	end
	return ids
end

function ROOMS.Sources(roomId)
	roomId = tonumber(roomId)
	local out, seen = {}, {}
	if (not roomId) then return out end
	for _, spec in ipairs({{'GET_WATCH_DEVICES', 'watch'}, {'GET_LISTEN_DEVICES', 'listen'}}) do
		for fragment in string.gmatch(SourceXml(roomId, spec[1]), '<source>(.-)</source>') do
			local id = tonumber(string.match(fragment, '<id>(.-)</id>'))
			if (id and not seen[id .. spec[2]]) then
				seen[id .. spec[2]] = true
				local name = string.match(fragment, '<name>(.-)</name>')
				table.insert(out, {id = id, name = name and U.XmlUnescape(name) or ROOMS.Name(id), type = spec[2]})
			end
		end
	end
	return out
end

-- Execution --------------------------------------------------------------------------

local function Send(roomId, command, params)
	U.Debug('room ' .. roomId .. ': ' .. command)
	if (C4 and C4.SendToDevice) then
		U.SafeCall(function() C4:SendToDevice(roomId, command, params or {}) end)
	end
end

-- A watch source is selected with SELECT_VIDEO_DEVICE, a listen source with
-- SELECT_AUDIO_DEVICE (the reference driver's SelectSourcePreset).
local function SelectSource(roomId, sourceId)
	local command
	if (SourceIds(roomId, 'GET_WATCH_DEVICES')[sourceId]) then
		command = 'SELECT_VIDEO_DEVICE'
	elseif (SourceIds(roomId, 'GET_LISTEN_DEVICES')[sourceId]) then
		command = 'SELECT_AUDIO_DEVICE'
		local digital = U.SafeCall(function() return C4:GetDeviceData(sourceId, 'digital_audio_support') end)
		if (string.lower(tostring(digital or '')) == 'true') then
			Send(sourceId, 'DEVICE_SELECTED', {idRoom = roomId})
			return true
		end
	else
		command = 'SELECT_VIDEO_DEVICE'
	end
	Send(roomId, command, {deviceid = sourceId})
	return true
end

-- Returns ok, err.
function ROOMS.Run(action)
	local a = ROOMS.Normalize(action)
	if (not a) then
		return false, 'not a valid action'
	end
	if (a.kind == 'event') then
		return true
	elseif (a.kind == 'source') then
		return SelectSource(a.room, a.source)
	end
	Send(a.room, a.command, a.params)
	if (a.release) then
		Send(a.room, a.release, a.params)
	end
	return true
end

function ROOMS.Describe(action)
	local a = ROOMS.Normalize(action)
	if (not a) then
		return '?'
	end
	if (a.kind == 'event') then
		return 'programming event only'
	end
	local text
	if (a.kind == 'source') then
		text = 'Select ' .. (a.label ~= '' and a.label or ROOMS.Name(a.source))
	else
		text = (a.label ~= '' and a.label or a.command)
		local parts = {}
		for k, v in pairs(a.params) do table.insert(parts, k .. '=' .. v) end
		table.sort(parts)
		if (#parts > 0) then text = text .. ' (' .. table.concat(parts, ', ') .. ')' end
	end
	if (a.roomName == '' and not ROOMS.cache) then
		ROOMS.List(false)
	end
	return text .. ' in ' .. (a.roomName ~= '' and a.roomName or ROOMS.Name(a.room))
end
