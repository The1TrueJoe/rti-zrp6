-- MQTT 3.1.1 packet encoder and stream decoder. No C4 calls here; the
-- connection manager (05_mqtt_client.lua) owns the socket.
--
-- Only what the ZRP-6 broker speaks: CONNECT with username/password,
-- SUBSCRIBE at QoS 0, PUBLISH at QoS 0, PINGREQ, DISCONNECT. Inbound QoS 1
-- PUBLISH is decoded (and acknowledged by the client) so the driver also
-- works against a stock broker in testing.

MQTT.CONNECT, MQTT.CONNACK, MQTT.PUBLISH, MQTT.PUBACK = 1, 2, 3, 4
MQTT.SUBSCRIBE, MQTT.SUBACK, MQTT.UNSUBSCRIBE, MQTT.UNSUBACK = 8, 9, 10, 11
MQTT.PINGREQ, MQTT.PINGRESP, MQTT.DISCONNECT = 12, 13, 14
MQTT.MAX_REMAINING = 268435455
-- Refuse to buffer anything larger than this from the box. Its biggest
-- packet is a ~560-byte rule listing; this only guards against garbage.
MQTT.MAX_INBOUND = 65536

MQTT.CONNACK_TEXT = {
	[0] = 'accepted',
	[1] = 'unacceptable protocol version',
	[2] = 'client identifier rejected',
	[3] = 'server unavailable',
	[4] = 'bad user name or password',
	[5] = 'not authorized (check username/password)',
}

-- Remaining Length: 7 bits per byte, least significant group first, top bit
-- set on every byte but the last. 1-4 bytes.
function MQTT.EncodeLength(n)
	if (n < 0 or n > MQTT.MAX_REMAINING) then
		error('remaining length out of range: ' .. tostring(n))
	end
	local out = {}
	repeat
		local digit = n % 128
		n = math.floor(n / 128)
		if (n > 0) then
			digit = digit + 128
		end
		table.insert(out, string.char(digit))
	until (n == 0)
	return table.concat(out)
end

-- Decodes a Remaining Length starting at position pos of s.
-- Returns value, bytesUsed; or nil, 'incomplete'; or nil, 'malformed'.
function MQTT.DecodeLength(s, pos)
	local value = 0
	local mult = 1
	for i = 0, 3 do
		local b = string.byte(s, pos + i)
		if (b == nil) then
			return nil, 'incomplete'
		end
		value = value + (b % 128) * mult
		if (b < 128) then
			return value, i + 1
		end
		mult = mult * 128
	end
	return nil, 'malformed'
end

local function U16(n)
	return string.char(math.floor(n / 256) % 256, n % 256)
end

function MQTT.Str(s)
	s = tostring(s or '')
	return U16(string.len(s)) .. s
end

local function Packet(firstByte, body)
	return string.char(firstByte) .. MQTT.EncodeLength(string.len(body)) .. body
end

-- opts: clientId, username, password, keepalive (s), cleanSession (default true)
function MQTT.Connect(opts)
	local flags = 0
	if (opts.cleanSession ~= false) then flags = flags + 2 end
	local payload = MQTT.Str(opts.clientId or '')
	if (opts.username ~= nil and opts.username ~= '') then
		flags = flags + 128
		payload = payload .. MQTT.Str(opts.username)
		if (opts.password ~= nil) then
			flags = flags + 64
			payload = payload .. MQTT.Str(opts.password)
		end
	end
	local keepalive = tonumber(opts.keepalive) or 30
	local variable = MQTT.Str('MQTT') .. string.char(4) .. string.char(flags) .. U16(keepalive)
	return Packet(MQTT.CONNECT * 16, variable .. payload)
end

-- topics: list of filter strings, all requested at QoS 0.
function MQTT.Subscribe(packetId, topics)
	local body = U16(packetId)
	for _, topic in ipairs(topics) do
		body = body .. MQTT.Str(topic) .. string.char(0)
	end
	return Packet(MQTT.SUBSCRIBE * 16 + 2, body)
end

function MQTT.Publish(topic, payload, retain)
	local first = MQTT.PUBLISH * 16 + (retain and 1 or 0)
	return Packet(first, MQTT.Str(topic) .. tostring(payload or ''))
end

function MQTT.PubAck(packetId)
	return Packet(MQTT.PUBACK * 16, U16(packetId))
end

function MQTT.PingReq()
	return string.char(MQTT.PINGREQ * 16, 0)
end

function MQTT.Disconnect()
	return string.char(MQTT.DISCONNECT * 16, 0)
end

-- Inbound -------------------------------------------------------------------

-- Returns sessionPresent (bool), returnCode (number) or nil on a short body.
function MQTT.DecodeConnack(body)
	if (string.len(body) < 2) then
		return nil
	end
	return (string.byte(body, 1) % 2) == 1, string.byte(body, 2)
end

-- Returns {topic, payload, qos, retain, dup, packetId} or nil, err.
function MQTT.DecodePublish(flags, body)
	local qos = math.floor(flags / 2) % 4
	local retain = (flags % 2) == 1
	local dup = flags >= 8
	if (string.len(body) < 2) then
		return nil, 'short publish'
	end
	local tlen = string.byte(body, 1) * 256 + string.byte(body, 2)
	if (2 + tlen > string.len(body)) then
		return nil, 'topic overruns packet'
	end
	local topic = string.sub(body, 3, 2 + tlen)
	local pos = 3 + tlen
	local packetId = nil
	if (qos > 0) then
		if (pos + 1 > string.len(body)) then
			return nil, 'missing packet id'
		end
		packetId = string.byte(body, pos) * 256 + string.byte(body, pos + 1)
		pos = pos + 2
	end
	return {
		topic = topic,
		payload = string.sub(body, pos),
		qos = qos,
		retain = retain,
		dup = dup,
		packetId = packetId,
	}
end

-- Stream parser. TCP delivers bytes, not packets: a read may hold half a
-- packet, or several. Feed() everything that arrives; it returns the packets
-- that are complete and keeps the rest for next time.
local Parser = {}
Parser.__index = Parser

function MQTT.NewParser()
	return setmetatable({buf = ''}, Parser)
end

function Parser:Reset()
	self.buf = ''
end

-- Returns a list of {type, flags, body}; or nil, err when the stream is
-- corrupt (the caller must drop the connection).
function Parser:Feed(data)
	if (data and data ~= '') then
		self.buf = self.buf .. data
	end
	local packets = {}
	local buf = self.buf
	local pos = 1
	local total = string.len(buf)
	while (total - pos + 1 >= 2) do
		local first = string.byte(buf, pos)
		local remaining, used = MQTT.DecodeLength(buf, pos + 1)
		if (remaining == nil) then
			if (used == 'malformed') then
				self.buf = ''
				return nil, 'malformed remaining length'
			end
			break -- incomplete length: wait for more
		end
		if (remaining > MQTT.MAX_INBOUND) then
			self.buf = ''
			return nil, 'packet too large (' .. remaining .. ' bytes)'
		end
		local header = 1 + used
		if (total - pos + 1 < header + remaining) then
			break -- body not all here yet
		end
		table.insert(packets, {
			type = math.floor(first / 16),
			flags = first % 16,
			body = string.sub(buf, pos + header, pos + header + remaining - 1),
		})
		pos = pos + header + remaining
	end
	if (pos > 1) then
		self.buf = string.sub(buf, pos)
	end
	return packets
end
