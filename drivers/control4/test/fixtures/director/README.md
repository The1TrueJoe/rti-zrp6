# Director REST fixtures

What the Remote Buttons tab reads from Director, for `test/ui/page_test.js`.

| File | Endpoint | Provenance |
|---|---|---|
| `rooms.json` | `GET /api/v1/rooms` | Captured from a live EA-3 (OS 3.3.3): The1TrueJoe/control4-fake-navigator `backend/demo/rooms.json` |
| `items.json` | `GET /api/v1/items?tree=false` | Subset of the same capture (`backend/demo/items.json`): the two rooms and some of their devices, fields trimmed |
| `ui_configuration.json` | `GET /api/v1/agents/ui_configuration` | Constructed in the shape Home Assistant's control4 integration reads (`experiences[].room_id/type/sources.source[].id`, tests/components/control4/fixtures/ui_configuration.json); source ids from the capture |
| `room-14-commands.json` | `GET /api/v1/items/14/commands` | **Constructed, not captured**: the shape pyControl4 `get_item_commands` users parse (`[{command, display, params: [{name, type, values: [{id, name}]}]}]`, randybritsch/c4-mcp `control4_gateway.py`, Ctrlable/ctrlable-control4 `keypad.py`). Replace with a capture when one is available. |
