#!/usr/bin/env python3
"""A scripted reference solution for task.md. It proves that grade.py can award 100.

Against a running TrenchBroom (builds the map and saves it to <out>/eval.map):

    python3 oracle.py --port <mcp port> --out <dir>

Or self-contained, as the opt-in ctest TrenchBroomMcpEvalOracle does: start TrenchBroom
in portable mode, build the map, grade it and require a score of 100:

    python3 oracle.py --app <TrenchBroom binary | TrenchBroom.app> [--timeout 60] [--keep]

Standard library only; reuses the client and launcher of ../mcp_e2e.py.
"""

import argparse
import json
import os
import sys

# don't leave __pycache__ directories in the source tree
sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
# explicit, so that the imports also work with python3 -I or -P
sys.path.insert(0, HERE)
sys.path.insert(1, os.path.join(HERE, ".."))

from mcp_e2e import Mcp, PortableApp, TestFailure, check  # noqa: E402

import grade  # noqa: E402

MATERIAL = "base"


def box(mn, mx):
    return {"min": list(mn), "max": list(mx)}


def operations():
    """The batch that builds both rooms, the corridor and the entities."""
    wall = grade.WALL
    (cmin, cmax) = grade.CORRIDOR
    room_a_wall = grade.ROOM_A[1][0]  # x of the inside of room A's +x wall
    room_b_wall = grade.ROOM_B[0][0]  # x of the inside of room B's -x wall

    # the corridor spans the gap between the outer faces of the two room walls
    x0 = room_a_wall + wall
    x1 = room_b_wall - wall
    return [
        # 0, 1: the rooms
        {"tool": "create_hollow_box", "arguments": {
            "inner_bounds": box(*grade.ROOM_A), "wall_thickness": wall, "material": MATERIAL}},
        {"tool": "create_hollow_box", "arguments": {
            "inner_bounds": box(*grade.ROOM_B), "wall_thickness": wall, "material": MATERIAL}},
        # 2, 3: cut the openings with a box of the corridor's interior; it touches the floors
        # and the side walls of the rooms, which are not cut
        {"tool": "create_box_brush", "arguments": {"bounds": box(cmin, cmax), "material": MATERIAL}},
        {"tool": "csg_subtract", "arguments": {"subtract_ids": ["$2"]}},
        # 4-7: corridor walls, floor and ceiling between the room walls
        {"tool": "create_box_brush", "arguments": {
            "bounds": box((x0, cmin[1] - wall, cmin[2]), (x1, cmin[1], cmax[2])), "material": MATERIAL}},
        {"tool": "create_box_brush", "arguments": {
            "bounds": box((x0, cmax[1], cmin[2]), (x1, cmax[1] + wall, cmax[2])), "material": MATERIAL}},
        {"tool": "create_box_brush", "arguments": {
            "bounds": box((x0, cmin[1] - wall, cmin[2] - wall), (x1, cmax[1] + wall, cmin[2])),
            "material": MATERIAL}},
        {"tool": "create_box_brush", "arguments": {
            "bounds": box((x0, cmin[1] - wall, cmax[2]), (x1, cmax[1] + wall, cmax[2] + wall)),
            "material": MATERIAL}},
        # 8-10: entities
        {"tool": "create_entity", "arguments": {
            "classname": "info_player_start", "origin": list(grade.PLAYER_START), "properties": {"angle": 0}}},
        {"tool": "create_entity", "arguments": {
            "classname": "light", "origin": list(grade.center(grade.ROOM_A)),
            "properties": {"light": grade.LIGHT_VALUE}}},
        {"tool": "create_entity", "arguments": {
            "classname": "light", "origin": list(grade.center(grade.ROOM_B)),
            "properties": {"light": grade.LIGHT_VALUE}}},
    ]


def build(mcp, out_dir):
    """Builds the map and saves it to <out_dir>/eval.map. Returns the map path."""
    mcp.call("new_map", game="Generic", format="Valve")
    starter = mcp.call("find_nodes", types=["brush"])["ids"]
    if starter:
        mcp.call("delete_nodes", ids=starter)

    mcp.call("checkpoint_create", name="empty")
    mcp.call("batch", operations=operations())

    overlaps = mcp.call("check_brush_overlaps")
    check(overlaps["pairs"] == [], "the oracle built overlapping brushes", None, overlaps)

    map_path = os.path.join(os.path.abspath(out_dir), "eval.map")
    mcp.call("save_map", path=map_path, overwrite=True)
    return map_path


def run_with_app(args):
    app = PortableApp(args.app, keep=args.keep, tmp_prefix="tb-mcp-eval-")
    exit_code = 1
    try:
        mcp = app.start(args.timeout)
        mcp.open_session()
        out_dir = os.path.join(app.tmpdir, "out")
        os.makedirs(out_dir)
        map_path = build(mcp, out_dir)
        report = grade.grade(mcp, map_path, pid=app.proc.pid, crash_dirs=[app.tmpdir])
        mcp.close()
        print(json.dumps(report, indent=2))
        check(app.alive(), "TrenchBroom exited")
        check(report["score"] == 100, f"the oracle scored {report['score']}, not 100")
        print("PASS: the oracle scored 100", flush=True)
        exit_code = 0
    except TestFailure as e:
        print(f"FAIL: {e}", file=sys.stderr, flush=True)
        app.print_log_tail()
    except Exception as e:  # noqa: BLE001 - report anything unexpected as a failure
        print(f"FAIL: unexpected {type(e).__name__}: {e}", file=sys.stderr, flush=True)
        app.print_log_tail()
    finally:
        if not app.stop():
            exit_code = 1
    return exit_code


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    target = parser.add_mutually_exclusive_group(required=True)
    target.add_argument("--app", help="start this TrenchBroom binary or TrenchBroom.app, build, grade")
    target.add_argument("--port", type=int, help="port of a running MCP server")
    parser.add_argument("--out", help="directory for eval.map (with --port)")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--keep", action="store_true", help="keep TrenchBroom running (with --app)")
    args = parser.parse_args()

    if args.app:
        return run_with_app(args)

    if not args.out:
        parser.error("--port needs --out")
    mcp = Mcp(args.port)
    try:
        mcp.open_session()
        print(build(mcp, args.out))
    except TestFailure as e:
        print(f"FAIL: {e}", file=sys.stderr)
        return 1
    finally:
        mcp.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
