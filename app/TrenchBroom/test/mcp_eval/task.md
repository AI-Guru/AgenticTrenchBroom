Using the `trenchbroom` MCP tools, create a new map (game Generic, format Valve) and build:

1. Room A, interior `[0,0,0]–[512,512,256]`, and Room B, interior `[640,0,0]–[1152,512,256]`. Every wall, floor and ceiling is 16 units thick. The rooms are connected by a corridor, interior `[512,192,0]–[640,320,128]`, with walls 16 thick. Openings in the room walls match the corridor cross-section exactly.
2. `info_player_start` in Room A at `[64,256,24]`, with `angle` 0.
3. One `light` (property `light` = 300) near the centre of each room, at least 32 units from any surface.
4. The whole structure is sealed: no leak to the void. There are no overlapping brushes, and every vertex is on an integer.
5. Save to `<OUT>/eval.map`. Verify your work with the evaluation tools before you finish.
