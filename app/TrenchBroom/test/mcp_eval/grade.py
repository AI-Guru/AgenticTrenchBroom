#!/usr/bin/env python3
"""Grades the map built for task.md through a running TrenchBroom's MCP server.

    python3 grade.py --port <mcp port> --map <OUT>/eval.map [--transcript transcript.jsonl]
        [--pid <TrenchBroom pid>] [--crash-dir <dir> ...]

Every check is deterministic and uses MCP tools only (no screenshots). The saved map is
opened with open_map and all geometry checks run on that reloaded document, so only what
was saved counts. Prints a JSON report {score, checks:[{name, pass, weight, details}],
metrics:{...}} and exits with 0 if the score is 100, else 1.

Standard library only; reuses the MCP client of ../mcp_e2e.py.
"""

import argparse
import json
import math
import os
import sys

# don't leave __pycache__ directories in the source tree
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from mcp_e2e import Mcp  # noqa: E402

ROOM_A = ((0, 0, 0), (512, 512, 256))
ROOM_B = ((640, 0, 0), (1152, 512, 256))
CORRIDOR = ((512, 192, 0), (640, 320, 128))
WALL = 16
PLAYER_START = (64, 256, 24)
LIGHT_VALUE = 300
LIGHT_CLEARANCE = 32

# the flood fill region (spec §12.3) and its cell size
FILL_MIN = (-64, -64, -64)
FILL_MAX = (1216, 576, 320)
FILL_CELL = 16

MAX_QUERY_POINTS = 200

VERIFICATION_TOOLS = {
    "validate_map", "query_point", "check_brush_overlaps", "checkpoint_create",
    "checkpoint_diff", "checkpoint_revert", "get_map_stats", "describe_region",
    "get_brush_geometry", "check_leaks", "check_walkable",
}

TOOL_PREFIX = "mcp__trenchbroom__"


class CheckFailed(Exception):
    pass


def require(condition, message):
    if not condition:
        raise CheckFailed(message)


class Grader:
    def __init__(self, mcp):
        self.mcp = mcp
        self.checks = []

    def run(self, name, weight, function):
        try:
            details = function()
            self.checks.append({"name": name, "pass": True, "weight": weight, "details": details or ""})
        except CheckFailed as e:
            self.checks.append({"name": name, "pass": False, "weight": weight, "details": str(e)})
        except Exception as e:  # noqa: BLE001 - a crash of a check fails that check only
            self.checks.append({"name": name, "pass": False, "weight": weight,
                                "details": f"{type(e).__name__}: {e}"})

    def score(self):
        return sum(c["weight"] for c in self.checks if c["pass"])

    # MCP helpers

    def solid(self, points):
        """Returns inside_solid for each point, querying at most 200 points per call."""
        result = []
        for i in range(0, len(points), MAX_QUERY_POINTS):
            chunk = [list(p) for p in points[i:i + MAX_QUERY_POINTS]]
            answers = self.mcp.call("query_point", points=chunk)["results"]
            require(len(answers) == len(chunk), "query_point returned the wrong number of results")
            result += [a["inside_solid"] for a in answers]
        return result

    def entities(self, classname):
        """Returns [(id, {key: value})] of the entities with the given classname."""
        ids = self.mcp.call("find_nodes", classname=classname)["ids"]
        ids = [i for i in ids if i != "world"]
        if not ids:
            return []
        nodes = self.mcp.call("get_nodes", ids=ids)["nodes"]
        return [(n["id"], {p["key"]: p["value"] for p in n.get("properties", [])}) for n in nodes]


def parse_vec(text):
    try:
        values = [float(v) for v in text.split()]
    except (AttributeError, ValueError):
        return None
    return tuple(values) if len(values) == 3 and all(math.isfinite(v) for v in values) else None


def parse_number(text):
    try:
        value = float(text)
    except (TypeError, ValueError):
        return None
    return value if math.isfinite(value) else None


def inside(point, box, margin=0):
    return all(box[0][i] + margin <= point[i] <= box[1][i] - margin for i in range(3))


def center(box):
    return tuple((box[0][i] + box[1][i]) / 2 for i in range(3))


def lattice(box, step=64):
    """Points on a lattice inside box, offset by half a step from its faces."""
    ranges = []
    for i in range(3):
        values = []
        v = box[0][i] + step / 2
        while v < box[1][i]:
            values.append(v)
            v += step
        if not values:
            values = [(box[0][i] + box[1][i]) / 2]
        ranges.append(values)
    return [(x, y, z) for x in ranges[0] for y in ranges[1] for z in ranges[2]]


def opening_boxes():
    """The openings of the corridor in the walls of both rooms, spanning the wall thickness."""
    (cmin, cmax) = CORRIDOR
    return [
        ((ROOM_A[1][0], cmin[1], cmin[2]), (ROOM_A[1][0] + WALL, cmax[1], cmax[2])),
        ((ROOM_B[0][0] - WALL, cmin[1], cmin[2]), (ROOM_B[0][0], cmax[1], cmax[2])),
    ]


def shell_points(room):
    """(point, expect_solid) pairs for the wall, floor and ceiling middles of a room."""
    (mn, mx) = room
    c = center(room)
    half = WALL / 2
    points = [
        (mn[0] - half, c[1], c[2]), (mx[0] + half, c[1], c[2]),
        (c[0], mn[1] - half, c[2]), (c[0], mx[1] + half, c[2]),
        (c[0], c[1], mn[2] - half), (c[0], c[1], mx[2] + half),
    ]
    result = []
    for p in points:
        opening = next((o for o in opening_boxes() if inside(p, o)), None)
        if opening is None:
            result.append((p, True))
        else:
            # the middle of this wall is in the corridor opening: the opening must be open
            # and the wall above it must be solid
            result.append((center(opening), False))
            result.append(((p[0], p[1], (opening[1][2] + mx[2]) / 2), True))
    return result


def check_file(map_path):
    require(os.path.isfile(map_path), f"{map_path} does not exist")
    with open(map_path, encoding="utf-8", errors="replace") as f:
        first_line = f.readline().strip()
    require(first_line.startswith("// Game: Generic"), f"first line is {first_line!r}")


def flood_fill(grader, start):
    """Returns (leaked, reached_cell_count). Flood fills empty 16-unit cells from start."""
    counts = [(FILL_MAX[i] - FILL_MIN[i]) // FILL_CELL for i in range(3)]

    def cell_center(cell):
        return tuple(FILL_MIN[i] + FILL_CELL * cell[i] + FILL_CELL / 2 for i in range(3))

    def on_boundary(cell):
        return any(cell[i] == 0 or cell[i] == counts[i] - 1 for i in range(3))

    start_cell = tuple(int((start[i] - FILL_MIN[i]) // FILL_CELL) for i in range(3))
    require(all(0 <= start_cell[i] < counts[i] for i in range(3)), "the start is outside the fill region")

    visited = {start_cell}
    frontier = [start_cell]
    reached = 0
    while frontier:
        solid = grader.solid([cell_center(c) for c in frontier])
        next_frontier = []
        for cell, is_solid in zip(frontier, solid):
            if is_solid:
                if cell == start_cell:
                    raise CheckFailed("the player start cell is solid")
                continue
            reached += 1
            if on_boundary(cell):
                return True, reached, cell_center(cell)
            for axis in range(3):
                for delta in (-1, 1):
                    neighbour = list(cell)
                    neighbour[axis] += delta
                    neighbour = tuple(neighbour)
                    if 0 <= neighbour[axis] < counts[axis] and neighbour not in visited:
                        visited.add(neighbour)
                        next_frontier.append(neighbour)
        frontier = next_frontier
    return False, reached, None


def transcript_metrics(path):
    metrics = {"tool_calls": 0, "tool_errors": 0, "verification_calls": 0, "tools": {},
               "other_tool_calls": 0, "other_tools": {}}
    if not path:
        return metrics
    if not os.path.isfile(path):
        metrics["error"] = f"no transcript at {path}"
        return metrics
    mcp_tool_use_ids = set()
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            try:
                event = json.loads(line)
            except ValueError:
                continue
            message = event.get("message") if isinstance(event, dict) else None
            content = message.get("content") if isinstance(message, dict) else None
            if not isinstance(content, list):
                continue
            for block in content:
                if not isinstance(block, dict):
                    continue
                if block.get("type") == "tool_use" and str(block.get("name", "")).startswith(TOOL_PREFIX):
                    tool = block["name"][len(TOOL_PREFIX):]
                    metrics["tool_calls"] += 1
                    metrics["tools"][tool] = metrics["tools"].get(tool, 0) + 1
                    if tool in VERIFICATION_TOOLS:
                        metrics["verification_calls"] += 1
                    mcp_tool_use_ids.add(block.get("id"))
                elif block.get("type") == "tool_use":
                    # e.g. a Bash grep of the saved map, which bypasses the MCP tools
                    tool = str(block.get("name", ""))
                    metrics["other_tool_calls"] += 1
                    metrics["other_tools"][tool] = metrics["other_tools"].get(tool, 0) + 1
                elif block.get("type") == "tool_result" and block.get("is_error") \
                        and block.get("tool_use_id") in mcp_tool_use_ids:
                    metrics["tool_errors"] += 1
    return metrics


def grade(mcp, map_path, transcript=None, pid=None, crash_dirs=()):
    """Runs all checks and returns the report."""
    grader = Grader(mcp)

    grader.run("File saved", 5, lambda: check_file(map_path))

    def reload_fidelity():
        before = mcp.call("get_map_stats")["content_digest"]
        require(os.path.isfile(map_path), "nothing to reload")
        opened = mcp.call("open_map", path=os.path.abspath(map_path))
        after = mcp.call("get_map_stats")["content_digest"]
        require(before == after, f"content_digest changed from {before} to {after} after reloading")
        return f"reloaded as {opened.get('document')}"

    # the remaining checks run on the reloaded document if the reload worked
    grader.run("Reload fidelity", 5, reload_fidelity)

    def player_start():
        starts = grader.entities("info_player_start")
        require(len(starts) == 1, f"{len(starts)} info_player_start entities")
        (node_id, props) = starts[0]
        origin = parse_vec(props.get("origin"))
        require(origin is not None, f"{node_id} has no valid origin")
        distance = math.dist(origin, PLAYER_START)
        require(distance <= 1, f"{node_id} origin {origin} is {distance:.1f} units from {PLAYER_START}")
        angle = parse_number(props.get("angle", "0"))
        require(angle is not None and angle % 360 == 0, f"{node_id} angle is {props.get('angle')}")
        require(not grader.solid([origin])[0], f"{node_id} is inside solid")
        return node_id

    grader.run("Player start", 10, player_start)

    def lights():
        lights = [(i, p) for (i, p) in grader.entities("light")
                  if parse_number(p.get("light")) == LIGHT_VALUE]
        require(len(lights) == 2, f"{len(lights)} lights with light={LIGHT_VALUE}")
        origins = [parse_vec(p.get("origin")) for (_, p) in lights]
        require(all(o is not None for o in origins), "a light has no valid origin")
        for room, name in ((ROOM_A, "A"), (ROOM_B, "B")):
            count = sum(1 for o in origins if inside(o, room, LIGHT_CLEARANCE))
            require(count == 1, f"{count} lights at least {LIGHT_CLEARANCE} units inside room {name}")
        require(not any(grader.solid(origins)), "a light is inside solid")
        return [i for (i, _) in lights]

    grader.run("Lights", 10, lights)

    def rooms_hollow():
        points = lattice(ROOM_A) + lattice(ROOM_B) + lattice(CORRIDOR)
        solid = [p for p, s in zip(points, grader.solid(points)) if s]
        require(not solid, f"{len(solid)} of {len(points)} interior points are solid, e.g. {solid[:5]}")
        return f"{len(points)} points"

    grader.run("Rooms hollow", 10, rooms_hollow)

    def walls_present():
        expected = shell_points(ROOM_A) + shell_points(ROOM_B)
        actual = grader.solid([p for p, _ in expected])
        wrong = [(p, e) for (p, e), a in zip(expected, actual) if a != e]
        require(not wrong, "wrong: " + ", ".join(
            f"{p} should {'be' if e else 'not be'} solid" for p, e in wrong))
        return f"{len(expected)} points"

    grader.run("Walls present", 10, walls_present)

    def doorway_open():
        points = [(x, 256, 64) for x in range(504, 649, 8)]
        solid = [p for p, s in zip(points, grader.solid(points)) if s]
        require(not solid, f"solid doorway points: {solid}")

    grader.run("Doorway open", 10, doorway_open)

    def sealed():
        starts = grader.entities("info_player_start")
        start = parse_vec(starts[0][1].get("origin")) if len(starts) == 1 else None
        start = start or PLAYER_START
        leaked, reached, leak = flood_fill(grader, start)
        require(not leaked, f"leak: reached {leak} at the boundary after {reached} cells")
        return f"{reached} empty cells reached"

    grader.run("Sealed", 15, sealed)

    def no_overlaps():
        overlaps = mcp.call("check_brush_overlaps")
        require(overlaps["pairs"] == [], f"{overlaps['total']} overlapping pairs, e.g. {overlaps['pairs'][:3]}")
        return f"{overlaps['brush_count']} brushes"

    grader.run("No overlaps", 10, no_overlaps)

    def clean_geometry():
        stats = mcp.call("get_map_stats")
        require(not stats["non_integer_brushes"], f"non-integer brushes: {stats['non_integer_brushes']}")
        require(not stats["invalid_brushes"], f"invalid brushes: {stats['invalid_brushes']}")
        by_type = mcp.call("validate_map")["by_type"]
        for issue_type in ("Non-integer vertices", "Missing entity classname"):
            require(not by_type.get(issue_type), f"{by_type.get(issue_type)} issues of type {issue_type}")

    grader.run("Clean geometry", 10, clean_geometry)

    def safety():
        require(mcp.request("ping") == {}, "ping failed")
        if pid is not None:
            try:
                os.kill(pid, 0)
            except OSError:
                raise CheckFailed(f"TrenchBroom (pid {pid}) is not running")
        directories = list(crash_dirs) + [os.path.dirname(os.path.abspath(map_path))]
        reports = [os.path.join(d, n) for d in directories if os.path.isdir(d)
                   for n in os.listdir(d) if "crash" in n.lower()]
        require(not reports, f"crash reports: {reports}")

    grader.run("Safety", 5, safety)

    return {
        "score": grader.score(),
        "checks": grader.checks,
        "metrics": transcript_metrics(transcript),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, required=True, help="port of the running MCP server")
    parser.add_argument("--map", required=True, help="the map file the agent saved")
    parser.add_argument("--transcript", help="claude stream-json transcript, for the metrics")
    parser.add_argument("--pid", type=int, help="TrenchBroom's process id, checked to be alive")
    parser.add_argument("--crash-dir", action="append", default=[], help="also look for crash reports here")
    args = parser.parse_args()

    mcp = Mcp(args.port)
    mcp.open_session()
    report = grade(mcp, args.map, args.transcript, args.pid, args.crash_dir)
    mcp.close()
    print(json.dumps(report, indent=2))
    return 0 if report["score"] == 100 else 1


if __name__ == "__main__":
    sys.exit(main())
