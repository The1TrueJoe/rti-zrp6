-- Constants and shared state.
--
-- Everything the driver keeps lives in a handful of module tables (ZRP, U,
-- MQTT, CLIENT, IO, ROOMS, REMOTES, DISCO, UI, PROPS) rather than in dozens of
-- top-level locals: Lua 5.1 / LuaJIT allow only 200 locals per chunk and 60
-- upvalues per function, and the concatenated driver.lua is one chunk.

local DRIVER_VERSION = '1.0.0'
local LOG_PREFIX = '[ZRP-6] '

local ZRP = {
	-- Hardware. The box has seven IR ports (port 7 is HIGH OUT), not eight.
	IR_PORTS = 7,
	SERIAL_PORTS = 2,
	RELAYS = 3,
	TRIGGERS = 3,
	CONTACTS = 3,

	-- Control binding IDs (1-999 are control bindings).
	BIND_IR_BASE = 100,        -- 101-107: IR_OUT provider, port in IR mode
	BIND_IR_SERIAL_BASE = 110, -- 111-117: RS_232 provider, port in one-way serial mode
	BIND_SERIAL_BASE = 200,    -- 201-202: RS_232 provider, the two RS-232 ports
	BIND_RELAY_BASE = 300,     -- 301-303: RELAY provider, relays
	BIND_TRIGGER_BASE = 310,   -- 311-313: RELAY provider, 12 V triggers
	BIND_CONTACT_BASE = 400,   -- 401-403: CONTACT_SENSOR provider, contact inputs
	NET_BINDING = 6001,        -- dynamic network connection to the box's broker
	MDNS_BINDING = 6002,       -- fallback mDNS listener (UDP to 224.0.0.251:5353, local port 5353)

	-- Box limits (docs/MQTT.md and firmware/src/broker.c).
	BAUDS = {1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 115200},
	SERIAL_FORMATS = {'8N1', '8E1', '8O1', '7E1', '7O1', '8N2'},
	-- The broker reads at most 768 bytes per poll and drops a packet that does
	-- not fit, so nothing we send may be bigger than this.
	MAX_PACKET = 760,
	-- The broker stores topics in 32-byte buffers (31 characters + NUL).
	MAX_TOPIC = 31,

	-- Timing.
	SEND_GAP_MS = 40,          -- spacing between packets so the box never sees two coalesced
	CONNECT_TIMEOUT_MS = 10000,
	CONNACK_TIMEOUT_MS = 10000,
	SETTLE_MS = 1500,          -- wait for retained state after SUBACK before reconciling
	BACKOFF_S = {2, 4, 8, 16, 30, 60},
	AUTH_BACKOFF_S = 30,       -- after CONNACK 4/5; the box refuses all logins for 2 s after a failure
	LEARN_TIMEOUT_S = 60,

	-- Static event IDs (driver.xml) and the start of the dynamic per-button events.
	EVENT_BUTTON_PRESSED = 1,
	EVENT_BUTTON_UNMAPPED = 2,
	EVENT_CONNECTED = 3,
	EVENT_DISCONNECTED = 4,
	EVENT_SERIAL_RX_BASE = 4,  -- 5, 6: RS-232 1/2 data received
	EVENT_DYNAMIC_BASE = 1000,

	PERSIST_REMOTES = 'ZRP6_REMOTES',
	PERSIST_IR_BINDINGS = 'ZRP6_IR_BINDINGS',
	PERSIST_PORT_ACKS = 'ZRP6_PORT_ACKS',
	PERSIST_BOX = 'ZRP6_BOX',
}

-- Module tables, declared up front so any file can refer to any other
-- (a local declared in a later file would not be in scope here).
local U, JSON, MQTT, CLIENT, IO, ROOMS, REMOTES, DISCO, UI, PROPS = {}, {}, {}, {}, {}, {}, {}, {}, {}, {}

local STATE = {
	debug = false,
	initDone = false,
	destroying = false,
	tickTimer = nil,
}
