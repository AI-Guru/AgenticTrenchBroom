#!/usr/bin/env python3
"""End-to-end test of TrenchBroom's MCP server.

Starts TrenchBroom in portable mode with --mcp-port, drives it over MCP Streamable HTTP
(JSON responses) and checks the results, including real screenshots. Uses the Python
standard library only, so it also serves as a reference client.

    python3 app/TrenchBroom/test/mcp_e2e.py --app <TrenchBroom binary | TrenchBroom.app>
        [--keep] [--timeout 60] [--port 0]

The portable config directory next to the binary is moved aside while the test runs and
restored afterwards; the user's regular preferences are not touched. Exit code 0 means
every check passed. On failure the step, the request and the response are printed.
"""

import argparse
import base64
import http.client
import json
import os
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time
import zlib

PROTOCOL_VERSION = "2025-06-18"

TIER0_TOOLS = [
    "list_games", "list_documents", "set_active_document", "new_map", "open_map",
    "save_map", "close_map", "get_map_info", "get_scene_tree", "get_nodes",
    "get_brush_geometry", "find_nodes", "find_faces", "get_selection", "validate_map",
    "get_map_stats", "checkpoint_create", "checkpoint_diff", "checkpoint_list",
    "checkpoint_delete", "export_map_text", "create_box_brush", "create_entity",
    "paste_map_text", "transform_nodes", "delete_nodes", "set_entity_properties",
    "set_face_attributes", "select_nodes", "undo", "redo", "get_camera", "set_camera",
    "focus_nodes", "screenshot", "screenshot_set", "get_conventions",
]


class TestFailure(Exception):
    pass


def check(condition, message, request=None, response=None):
    if not condition:
        details = message
        if request is not None:
            details += "\n  request:  " + json.dumps(request)[:2000]
        if response is not None:
            details += "\n  response: " + json.dumps(response)[:4000]
        raise TestFailure(details)


class Mcp:
    """A minimal MCP client for the Streamable HTTP transport with JSON responses."""

    def __init__(self, port):
        self.port = port
        self.session_id = None
        self.protocol_version = None
        self.next_id = 1
        self.connection = None

    def _connection(self):
        if self.connection is None:
            self.connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=120)
        return self.connection

    def raw(self, method, body=None, headers=None, session=True, host=None):
        """Sends a request and returns (status, headers, body bytes)."""
        all_headers = {
            "Host": host or f"127.0.0.1:{self.port}",
            "Accept": "application/json, text/event-stream",
        }
        if body is not None:
            all_headers["Content-Type"] = "application/json"
        if session and self.session_id:
            all_headers["Mcp-Session-Id"] = self.session_id
        if session and self.protocol_version:
            all_headers["MCP-Protocol-Version"] = self.protocol_version
        all_headers.update(headers or {})
        all_headers = {k: v for k, v in all_headers.items() if v is not None}

        data = body if isinstance(body, bytes) or body is None else json.dumps(body).encode()
        for attempt in range(2):
            connection = self._connection()
            try:
                connection.putrequest(method, "/mcp", skip_host=True, skip_accept_encoding=True)
                for key, value in all_headers.items():
                    connection.putheader(key, value)
                connection.putheader("Content-Length", str(len(data or b"")))
                connection.endheaders(data)
                response = connection.getresponse()
                payload = response.read()
                if response.getheader("Connection", "").lower() == "close":
                    self.close()
                return response.status, {k.lower(): v for k, v in response.getheaders()}, payload
            except (ConnectionError, http.client.HTTPException):
                self.close()
                if attempt == 1:
                    raise

    def close(self):
        if self.connection is not None:
            self.connection.close()
            self.connection = None

    def post(self, obj, headers=None, session=True, host=None):
        status, response_headers, payload = self.raw("POST", obj, headers, session, host)
        body = json.loads(payload) if payload else None
        return status, response_headers, body

    def request(self, method, params=None):
        message = {"jsonrpc": "2.0", "id": self.next_id, "method": method}
        self.next_id += 1
        if params is not None:
            message["params"] = params
        status, _, body = self.post(message)
        check(status == 200, f"{method}: HTTP {status}", message, body)
        check("error" not in body, f"{method}: JSON-RPC error", message, body)
        return body["result"]

    def initialize(self):
        message = {
            "jsonrpc": "2.0", "id": self.next_id, "method": "initialize",
            "params": {
                "protocolVersion": PROTOCOL_VERSION,
                "capabilities": {},
                "clientInfo": {"name": "mcp_e2e", "version": "1.0"},
            },
        }
        self.next_id += 1
        status, headers, body = self.post(message, session=False)
        return status, headers, body, message

    def open_session(self):
        """Initializes a session and sends notifications/initialized."""
        status, headers, body, message = self.initialize()
        check(status == 200 and "mcp-session-id" in headers, "initialize failed", message, body)
        self.session_id = headers["mcp-session-id"]
        self.protocol_version = body["result"]["protocolVersion"]
        check(self.notify("notifications/initialized") == 202, "notifications/initialized failed")

    def notify(self, method, params=None):
        message = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            message["params"] = params
        status, _, _ = self.post(message)
        return status

    def call_raw(self, tool, args):
        message = {
            "jsonrpc": "2.0", "id": self.next_id, "method": "tools/call",
            "params": {"name": tool, "arguments": args},
        }
        self.next_id += 1
        status, _, body = self.post(message)
        check(status == 200, f"{tool}: HTTP {status}", message, body)
        check("result" in body, f"{tool}: JSON-RPC error", message, body)
        return message, body["result"]

    def call(self, tool, **args):
        """Calls a tool, requires success and returns its structured content."""
        message, result = self.call_raw(tool, args)
        check(not result.get("isError"), f"{tool} failed", message, result)
        return result["structuredContent"]

    def call_full(self, tool, **args):
        """Calls a tool, requires success and returns the whole CallToolResult."""
        message, result = self.call_raw(tool, args)
        check(not result.get("isError"), f"{tool} failed", message, result)
        return result

    def call_err(self, tool, **args):
        """Calls a tool, requires a tool error and returns its code."""
        message, result = self.call_raw(tool, args)
        check(result.get("isError"), f"{tool} should have failed", message, result)
        return result["structuredContent"]["error"]["code"]


# PNG decoding (stdlib only)

def decode_png(data):
    """Decodes an 8 bit RGB or RGBA PNG. Returns (width, height, channels, rows)."""
    check(data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG image")
    pos = 8
    width = height = None
    bit_depth = color_type = interlace = None
    idat = b""
    while pos < len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            width, height, bit_depth, color_type, _, _, interlace = struct.unpack(">IIBBBBB", chunk)
        elif kind == b"IDAT":
            idat += chunk
        elif kind == b"IEND":
            break
    check(bit_depth == 8 and color_type in (2, 6) and interlace == 0,
          f"unsupported PNG (depth {bit_depth}, color type {color_type}, interlace {interlace})")
    channels = 3 if color_type == 2 else 4
    raw = zlib.decompress(idat)
    stride = width * channels
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        kind = raw[offset]
        line = bytearray(raw[offset + 1:offset + 1 + stride])
        offset += 1 + stride
        for i in range(stride):
            left = line[i - channels] if i >= channels else 0
            up = previous[i]
            up_left = previous[i - channels] if i >= channels else 0
            if kind == 1:
                line[i] = (line[i] + left) & 0xFF
            elif kind == 2:
                line[i] = (line[i] + up) & 0xFF
            elif kind == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif kind == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                predictor = left if pa <= pb and pa <= pc else (up if pb <= pc else up_left)
                line[i] = (line[i] + predictor) & 0xFF
        rows.append(bytes(line))
        previous = line
    return width, height, channels, rows


def distinct_colors(png, limit=64):
    width, height, channels, rows = png
    colors = set()
    for row in rows[::max(1, height // 64)]:
        for x in range(0, width, max(1, width // 64)):
            colors.add(row[x * channels:x * channels + 3])
            if len(colors) >= limit:
                return colors
    return colors


def image_from_result(result, index=0):
    images = [c for c in result["content"] if c.get("type") == "image"]
    check(len(images) > index, "no image content", None, {"content_types": [c.get("type") for c in result["content"]]})
    check(images[index]["mimeType"] == "image/png", "image is not a PNG")
    return base64.b64decode(images[index]["data"])


def same_camera(a, b, tolerance=1e-4):
    """Compares cameras; restoring recomputes the up vector, which may round."""
    return all(
        abs(x - y) <= tolerance for key in ("position", "direction", "up") for x, y in zip(a[key], b[key])
    ) and a["fov"] == b["fov"]


# Test driver

def resolve_binary(app):
    if app.endswith(".app") or os.path.isdir(app):
        name = os.path.splitext(os.path.basename(app.rstrip("/")))[0]
        return os.path.join(app, "Contents", "MacOS", name)
    return app


def free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Steps:
    def __init__(self, proc):
        self.proc = proc
        self.current = None

    def step(self, name):
        if self.current is not None:
            self.alive()
            print(f"  ok  {self.current}", flush=True)
        self.current = name

    def alive(self):
        check(self.proc.poll() is None, f"TrenchBroom exited with code {self.proc.returncode}")

    def finish(self):
        self.step(None)


def wait_for_server(mcp, proc, timeout):
    deadline = time.time() + timeout
    last_error = None
    while time.time() < deadline:
        if proc.poll() is not None:
            raise TestFailure(f"TrenchBroom exited early with code {proc.returncode}")
        try:
            result = mcp.initialize()
            mcp.close()
            return result
        except (OSError, http.client.HTTPException) as e:
            last_error = e
            mcp.close()
            time.sleep(0.25)
    raise TestFailure(f"the MCP server did not answer within {timeout} s: {last_error}")


def run_protocol_checks(mcp, steps):
    steps.step("protocol: initialize")
    status, headers, body, message = mcp.initialize()
    check(status == 200, "initialize: expected 200", message, body)
    check("mcp-session-id" in headers, "initialize: no Mcp-Session-Id header", message, body)
    check(body["result"]["protocolVersion"] == PROTOCOL_VERSION, "initialize: protocol version", message, body)
    check("instructions" in body["result"], "initialize: no instructions", message, body)
    check("Call get_conventions first" in body["result"]["instructions"],
          "initialize: instructions say to call get_conventions first", message, body)
    mcp.session_id = headers["mcp-session-id"]
    mcp.protocol_version = body["result"]["protocolVersion"]

    steps.step("protocol: notifications/initialized")
    check(mcp.notify("notifications/initialized") == 202, "notification: expected 202")

    steps.step("protocol: tools/list")
    tools = mcp.request("tools/list")["tools"]
    names = {tool["name"] for tool in tools}
    missing = [name for name in TIER0_TOOLS if name not in names]
    check(not missing, f"tools/list: missing tools {missing}")
    for tool in tools:
        check("annotations" in tool, f"tools/list: {tool['name']} has no annotations")
        check(tool["annotations"].get("openWorldHint") is False, f"{tool['name']}: openWorldHint")
        check(tool["inputSchema"].get("additionalProperties") is False,
              f"tools/list: {tool['name']} allows additional properties")

    steps.step("protocol: HTTP errors")
    status, _, _ = mcp.raw("GET")
    check(status == 405, f"GET: expected 405, got {status}")
    ping = {"jsonrpc": "2.0", "id": 9999, "method": "ping"}
    status, _, _ = mcp.post(ping, headers={"Origin": "http://evil.example"})
    check(status == 403, f"evil Origin: expected 403, got {status}")
    status, _, _ = mcp.post(ping, host=f"attacker:{mcp.port}")
    check(status == 403, f"evil Host: expected 403, got {status}")
    status, _, _ = mcp.post(ping, session=False)
    check(status == 400, f"no session: expected 400, got {status}")
    status, _, _ = mcp.post(ping, headers={"Mcp-Session-Id": "bogus"})
    check(status == 404, f"bogus session: expected 404, got {status}")
    status, _, body = mcp.post(b"{not json")
    check(status == 400 and body and body["error"]["code"] == -32700,
          f"malformed body: expected 400/-32700, got {status}", None, body)
    status, _, _ = mcp.post(ping, headers={"MCP-Protocol-Version": "1999-01-01"})
    check(status == 400, f"bad protocol version: expected 400, got {status}")
    check(mcp.request("ping") == {}, "ping")


def run_scenario(mcp, steps, tmpdir):
    steps.step("list_games")
    games = mcp.call("list_games")["games"]
    check(any(g["name"] == "Generic" for g in games), "list_games: no Generic game", None, games)

    steps.step("no document yet")
    check(mcp.call_err("get_map_info") == "no_document", "get_map_info without a map")

    steps.step("get_conventions before a map is open")
    conventions = mcp.call("get_conventions")
    check(conventions["document"] is None and conventions["up_axis"] == "+z"
          and conventions["player"]["step_height"] == 18 and "grid_size" not in conventions
          and "new_map" in conventions["next_step"] and conventions["games"] == games,
          "get_conventions without a document", None, conventions)
    check("light" in conventions["entity_definitions_note"] and "Quake" in conventions["entity_definitions_note"],
          "get_conventions: Generic has no light definition", None, conventions)
    generic = mcp.call("get_conventions", game="Generic")["game"]
    check(generic["name"] == "Generic" and generic["entity_definitions"]["default"] == "Generic.fgd"
          and generic["entity_definitions"]["default_available"] and "Quake" in generic["note"]
          and generic["material_default"] == "__TB_empty",
          "get_conventions game Generic", None, generic)
    quake = mcp.call("get_conventions", game="Quake")["game"]
    check(quake["material_property"] == "wad" and quake["default_format"] == "Valve"
          and quake["entity_definitions"]["default"] == "Quake.fgd"
          and quake["entity_definitions"]["default_available"] and "note" not in quake,
          "get_conventions game Quake", None, quake)
    check(mcp.call_err("get_conventions", game="Nope") == "unknown_game", "get_conventions unknown game")

    steps.step("validate_map marks the empty wad of a new Quake map as expected")
    mcp.call("new_map", game="Quake")
    issues = mcp.call("validate_map")
    wad = [item for item in issues["items"] if item.get("property") == "wad"]
    check(len(wad) == 1 and wad[0]["node"] == "world" and wad[0]["expected_for_new_map"]
          and issues["expected_count"] == issues["total"] == 1 and "note" in issues,
          "validate_map: wad expected", None, issues)
    mcp.call("create_entity", classname="light", origin=[0, 0, 64])
    check(mcp.call("validate_map")["total"] == 1, "validate_map: light is defined in Quake")
    # setting the wad reloads the entity definitions; this used to crash set_entity_properties
    mcp.call("set_entity_properties", ids=["world"], set={"wad": "base.wad"})
    issues = mcp.call("validate_map")
    check(issues["total"] == 0 and issues["expected_count"] == 0, "validate_map: wad set", None, issues)
    check(mcp.call("get_conventions")["map_format"] == "Valve", "get_conventions with a Quake map")
    mcp.call("close_map", discard_changes=True)
    check(mcp.call("get_conventions")["document"] is None, "get_conventions after closing the map")

    steps.step("new_map")
    created = mcp.call("new_map", game="Generic", format="Valve")
    check(created["game"] == "Generic" and created["format"] == "Valve", "new_map: game/format", None, created)
    check("scene" in created and created["document"].startswith("d"), "new_map: scene/document", None, created)
    documents = mcp.call("list_documents")["documents"]
    check(len(documents) == 1 and documents[0]["target"], "list_documents", None, documents)
    check(mcp.call("set_active_document", document=documents[0]["document"])["document"] == documents[0]["document"],
          "set_active_document")
    check(mcp.call_err("set_active_document", document="d999") == "unknown_document", "unknown document")

    stats = mcp.call("get_map_stats")
    check(stats["counts"]["world_brushes"] >= 0, "get_map_stats", None, stats)
    brushes = mcp.call("find_nodes", types=["brush"])["ids"]
    if brushes:
        mcp.call("delete_nodes", ids=brushes)
    check(mcp.call("get_map_stats")["counts"]["world_brushes"] == 0, "starter brushes deleted")

    steps.step("checkpoint_create")
    mcp.call("checkpoint_create", name="empty")

    steps.step("create_box_brush + get_brush_geometry")
    box = mcp.call("create_box_brush", bounds={"min": [0, 0, 0], "max": [64, 64, 64]}, material="base")
    box_id = box["id"]
    geometry = mcp.call("get_brush_geometry", ids=[box_id])["brushes"][0]
    check(geometry["vertex_count"] == 8 and geometry["face_count"] == 6, "box: counts", None, geometry)
    check(geometry["bounds"] == {"min": [0, 0, 0], "max": [64, 64, 64]}, "box: bounds", None, geometry["bounds"])
    check(geometry["volume"] == 262144, "box: volume", None, geometry["volume"])
    check(all(geometry["valid"].values()), "box: valid", None, geometry["valid"])

    steps.step("create_entity + set_entity_properties")
    light = mcp.call("create_entity", classname="light", origin=[32, 32, 96], properties={"light": 300})["id"]
    mcp.call("set_entity_properties", ids=[light], set={"light": 400})
    node = mcp.call("get_nodes", ids=[light])["nodes"][0]
    values = {p["key"]: p["value"] for p in node["properties"]}
    check(values.get("light") == "400", "light property", None, node)

    steps.step("validate_map")
    issues = mcp.call("validate_map")
    check(not any(item["type"] == "Missing entity classname" for item in issues["items"]),
          "validate_map: unexpected missing classname", None, issues)
    # Generic has no definition for light; the issue is marked as expected
    missing = [item for item in issues["items"] if item["type"] == "Missing entity definition"]
    check(len(missing) == 1 and missing[0]["node"] == light and missing[0]["expected_for_new_map"]
          and "Quake" in missing[0]["expected_reason"] and issues["expected_count"] == 1,
          "validate_map: light in Generic is expected", None, issues)

    steps.step("checkpoint_diff")
    diff = mcp.call("checkpoint_diff", **{"from": "empty"})
    check(diff["added_count"] == 2, "checkpoint_diff: 2 added", None, diff)

    steps.step("transform_nodes + undo/redo")
    mcp.call("transform_nodes", ids=[box_id], translate=[16, 0, 0])
    check(mcp.call("get_nodes", ids=[box_id])["nodes"][0]["bounds"]["min"][0] == 16, "translated")
    undone = mcp.call("undo")
    check(undone["undone"] == ["MCP: Transform Nodes"], "undo label", None, undone)
    check(mcp.call("get_nodes", ids=[box_id])["nodes"][0]["bounds"]["min"][0] == 0, "undone")
    mcp.call("redo")
    mcp.call("undo")

    steps.step("redo with nothing to redo")
    mcp.call("redo")
    check(mcp.call_err("redo") == "nothing_to_redo", "redo: nothing_to_redo")
    mcp.call("undo")

    steps.step("export_map_text")
    exported = mcp.call("export_map_text")["text"]
    check('"classname" "light"' in exported, "export_map_text: light", None, exported[:500])

    steps.step("save_map")
    map_path = os.path.join(tmpdir, "e2e.map")
    saved = mcp.call("save_map", path=map_path)
    check(saved["bytes"] > 0 and os.path.isfile(map_path), "save_map: file", None, saved)
    with open(map_path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    check("// Game: Generic" in text and '"classname" "light"' in text, "save_map: content", None, text[:500])
    check(mcp.call_err("save_map", path=map_path) == "file_exists", "save_map: file_exists")
    check(mcp.call_err("save_map", path=os.path.join(tmpdir, "missing", "x.map")) == "io_error",
          "save_map: missing directory")
    check(mcp.call_err("save_map", path="relative.map") == "invalid_argument", "save_map: relative path")
    mcp.call("save_map")

    steps.step("close_map + open_map")
    digest = mcp.call("get_map_stats")["content_digest"]
    closed = mcp.call("close_map")
    check(closed["closed"] and closed["remaining_documents"] == 0, "close_map", None, closed)
    check(mcp.call_err("get_map_info") == "no_document", "no document after close")
    opened = mcp.call("open_map", path=map_path)
    check(opened["game"] == "Generic", "open_map: game from header", None, opened)
    stats = mcp.call("get_map_stats")
    check(stats["counts"]["world_brushes"] == 1 and stats["counts"]["point_entities"] == 1,
          "open_map: counts", None, stats["counts"])
    check(stats["content_digest"] == digest, "open_map: digest after reload")
    check(mcp.call_err("open_map", path=os.path.join(tmpdir, "nope.map")) == "io_error", "open_map: missing file")
    check(mcp.call_err("new_map", game="NoSuchGame") == "unknown_game", "new_map: unknown game")
    check(mcp.call_err("new_map", game="Generic", format="Quake3") == "unsupported_format",
          "new_map: unsupported format")

    steps.step("paste_map_text")
    pasted = mcp.call("paste_map_text", text=exported, offset=[0, 128, 0])
    check(len(pasted["ids"]) >= 2, "paste: created ids", None, pasted)
    check(mcp.call("get_map_stats")["counts"]["world_brushes"] == 2, "paste: 2 brushes")
    if "check_brush_overlaps" in mcp.tool_names:
        check(mcp.call("check_brush_overlaps")["pairs"] == [], "no overlaps")
    mcp.call("undo")

    return map_path


def run_helper_checks(mcp, steps):
    """Blocks out a room with stairs using the agent helper tools, then reverts it."""
    steps.step("get_conventions")
    conventions = mcp.call("get_conventions")
    check(conventions["up_axis"] == "+z" and conventions["player"]["step_height"] == 18
          and conventions["document"] is not None and conventions["map_format"] == "Valve"
          and conventions["grid_size"] > 0 and conventions["game"]["name"] == "Generic",
          "get_conventions", None, conventions)
    mcp.call("checkpoint_create", name="before_helpers")
    digest = mcp.call("get_map_stats")["content_digest"]

    steps.step("create_room with a door")
    room = mcp.call("create_room", inner_bounds={"min": [256, 0, 0], "max": [768, 512, 256]},
                    material="base", openings=[{"side": "+x", "width": 64, "height": 96}], group_name="hall")
    check(len(room["walls"]["+x"]) == 3 and all(len(room["walls"][s]) == 1 for s in ["-x", "+y", "-y"]),
          "create_room: walls", None, room)
    door = room["openings"][0]["bounds"]
    check(door == {"min": [768, 224, 0], "max": [784, 288, 96]}, "create_room: door bounds", None, door)
    floor_bounds = mcp.call("get_nodes", ids=[room["floor"]])["nodes"][0]["bounds"]
    check(floor_bounds == {"min": [240, -16, -16], "max": [784, 528, 0]}, "create_room: floor", None, floor_bounds)
    points = mcp.call("query_point", points=[[776, 256, 48], [776, 100, 48], [512, 256, 128]])["results"]
    check([p["inside_solid"] for p in points] == [False, True, False], "create_room: door is open", None, points)

    steps.step("find_faces + set_face_attributes face_filter")
    inner = {"min": [256, 0, 0], "max": [768, 512, 256]}
    floor_filter = {"facing": "up", "exposed_only": True, "inside": inner}
    floors = mcp.call("find_faces", ids=[room["group"]], **floor_filter)
    check([(f["brush"], f["side"], f["area"]) for f in floors["faces"]] == [(room["floor"], "+z", 544 * 544)]
          and floors["faces"][0]["normal"] == [0, 0, 1], "find_faces: the walkable floor surface", None, floors)
    ceilings = mcp.call("find_faces", ids=[room["group"]], facing="down", exposed_only=True, inside=inner)
    check([(f["brush"], f["side"]) for f in ceilings["faces"]] == [(room["ceiling"], "-z")],
          "find_faces: the visible ceiling surface", None, ceilings)
    walls = mcp.call("find_faces", ids=[room["group"]], facing="horizontal", exposed_only=True, inside=inner)
    wall_ids = {i for side in ["+x", "-x", "+y", "-y"] for i in room["walls"][side]}
    check(len(walls["faces"]) == 6 and all(f["brush"] in wall_ids for f in walls["faces"]),
          "find_faces: the inner wall surfaces (3 pieces on the door wall)", None, walls)
    painted = mcp.call("set_face_attributes", brushes=[room["group"]], face_filter=floor_filter, material="floor_01")
    check(painted["faces_changed"] == 1 and painted["undo_label"] == "MCP: Set Face Attributes",
          "set_face_attributes face_filter: one face in one undo step", None, painted)
    check([(f["brush"], f["side"]) for f in mcp.call("find_faces", material="floor_01")["faces"]]
          == [(room["floor"], "+z")], "set_face_attributes face_filter: only the floor surface changed")
    noop = mcp.call("set_face_attributes", brushes=[room["group"]], face_filter={"material": "none*"}, material="x")
    check(noop["faces_changed"] == 0 and noop["undo_label"] is None, "face_filter without matches is rolled back",
          None, noop)

    steps.step("check_leaks")
    leaky = mcp.call("check_leaks", start_points=[[512, 256, 128]])
    leak = leaky["leaks"][0] if leaky["leaks"] else {}
    check(not leaky["sealed"] and leak.get("path", [None])[0] == [512, 256, 128]
          and leak["path"][-1] == leak["exit_point"] and leak["exit_point"][0] > 784,
          "check_leaks: leaks through the door", None, leaky)
    plug = mcp.call("create_box_brush", bounds=door, material="base")["id"]
    sealed = mcp.call("check_leaks", start_points=[[512, 256, 128]])
    check(sealed["sealed"] and sealed["leaks"] == [] and sealed["start_points_checked"] == 1,
          "check_leaks: sealed with the door plugged", None, sealed)
    mcp.call("undo")
    check(mcp.call_err("get_nodes", ids=[plug]) in ("stale_node", "unknown_node"), "check_leaks: plug undone")

    steps.step("create_stairs")
    stairs = mcp.call("create_stairs", start=[400, 100, 0], direction="+y", steps=6, material="base")
    check(len(stairs["ids"]) == 6 and stairs["top"] == [400, 196, 48], "create_stairs: top", None, stairs)
    check(stairs["bounds"] == {"min": [368, 100, 0], "max": [432, 196, 48]}, "create_stairs: bounds", None, stairs)
    check(mcp.call("check_brush_overlaps")["pairs"] == [], "room and stairs do not overlap")

    steps.step("drop_to_floor")
    light = mcp.call("create_entity", classname="light", origin=[600, 256, 200])["id"]
    crate = mcp.call("create_box_brush", bounds={"min": [384, 180, 150], "max": [416, 196, 182]},
                     material="base")["id"]
    dropped = mcp.call("drop_to_floor", ids=[light, crate])
    moved = {m["id"]: m for m in dropped["moved"]}
    check(moved.get(light, {}).get("on") == room["floor"], "drop_to_floor: light on floor", None, dropped)
    check(moved.get(crate, {}).get("on") == stairs["ids"][-1], "drop_to_floor: crate on top step", None, dropped)
    crate_bounds = mcp.call("get_nodes", ids=[crate])["nodes"][0]["bounds"]
    check(crate_bounds == {"min": [384, 180, 48], "max": [416, 196, 80]}, "drop_to_floor: crate bounds", None,
          crate_bounds)
    light_bounds = mcp.call("get_nodes", ids=[light])["nodes"][0]["bounds"]
    check(light_bounds["min"][2] == 0, "drop_to_floor: light rests on z 0", None, light_bounds)

    steps.step("align_nodes + describe_region")
    aligned = mcp.call("align_nodes", ids=[crate], to=stairs["ids"][-1], side="+x")
    check(aligned["moved"] == [{"id": crate, "delta": [16, 0, 0]}], "align_nodes", None, aligned)
    region = mcp.call("describe_region", bounds={"min": [256, 0, 0], "max": [768, 512, 256]})
    check(region["counts"]["entities"] == 1 and region["center_solid"] is False, "describe_region", None, region)

    steps.step("batch dry_run")
    before = mcp.call("export_map_text")["text"]
    preview = mcp.call("batch", dry_run=True, operations=[
        {"tool": "create_box_brush", "arguments": {"bounds": {"min": [300, 300, 0], "max": [332, 332, 32]}}},
        {"tool": "transform_nodes", "arguments": {"ids": ["$0", crate], "translate": [0, 0, 8]}},
        {"tool": "validate_map"}])
    check(preview["dry_run"] is True and len(preview["results"]) == 3, "dry_run: results", None, preview)
    check(mcp.call("export_map_text")["text"] == before, "dry_run: map text unchanged")

    steps.step("create_opening + create_corridor")
    # a second room, built at the origin and moved next to the hall as an agent would; it stays on the 16 grid
    # because check_leaks reports walls off its 8 unit cells as leaks
    annex = mcp.call("create_room", inner_bounds={"min": [0, 0, 0], "max": [256, 256, 128]},
                     material="base", group_name="annex")
    mcp.call("transform_nodes", ids=[annex["group"]], translate=[896, 128, 0])
    cut = mcp.call("create_opening", wall=annex["group"], side="-x", width=64, height=96)
    check(cut["opening"] == {"side": "-x", "bounds": {"min": [880, 224, 0], "max": [896, 288, 96]}}
          and len(cut["pieces"]) == 1 and len(next(iter(cut["pieces"].values()))) == 3 and cut["removed"] == [],
          "create_opening: door in the moved room", None, cut)
    corridor = mcp.call("create_corridor", **{"from": room["openings"][0], "to": cut["opening"]})
    check(corridor["shape"] == "straight" and len(corridor["ids"]) == 4
          and corridor["interior_bounds"] == [{"min": [784, 224, 0], "max": [880, 288, 96]}]
          and corridor["undo_label"] == "MCP: Create Corridor", "create_corridor: straight", None, corridor)
    check(mcp.call("check_brush_overlaps")["pairs"] == [], "corridor does not overlap the rooms")
    points = mcp.call("query_point", points=[[832, 256, 48], [776, 256, 48], [888, 256, 48]])["results"]
    check(not any(p["inside_solid"] for p in points), "corridor and doors are open", None, points)
    joined = mcp.call("check_leaks", start_points=[[512, 256, 128], [1024, 256, 64]])
    check(joined["sealed"] and joined["leaks"] == [], "check_leaks: two rooms and the corridor are sealed", None,
          joined)

    steps.step("check_walkable")
    # from the hall through the corridor into the annex, and up the stairs (8 unit steps)
    walk = mcp.call("check_walkable", **{"from": [512, 256, 0], "to": [[1024, 256, 0], [384, 152, 40],
                                                                       [400, 150, 10]]})
    annex_walk, stairs_walk, buried = walk["targets"]
    check(walk["reachable_count"] == 2 and annex_walk["reachable"] and annex_walk["path"][0] == [512, 256, 0]
          and annex_walk["path"][-1] == [1024, 256, 0] and annex_walk["path_length"] == 512,
          "check_walkable: through the corridor", None, walk)
    check(stairs_walk["reachable"] and stairs_walk["standing_point"] == [384, 152, 40]
          and any(0 < p[2] < 40 for p in stairs_walk["path"]), "check_walkable: up the stairs", None, walk)
    check(not buried["reachable"] and buried["blocker"]["kind"] == "not_standable"
          and buried["blocker"]["details"]["reason"] == "inside_solid", "check_walkable: inside the stairs", None,
          walk)
    # the steps are 16 deep, so a hull on the 4th step (z 32) also rests on the 5th (z 40)
    on_step = mcp.call("check_walkable", **{"from": [384, 152, 32], "to": [[1024, 256, 0], [384, 152, 32]]})
    check(on_step["from"]["standing_point"] == [384, 152, 40] and on_step["reachable_count"] == 2
          and on_step["targets"][1]["standing_point"] == [384, 152, 40],
          "check_walkable: a point on a step narrower than the hull", None, on_step)
    low = mcp.call("check_walkable", step_height=4, **{"from": [512, 256, 0], "to": [[384, 152, 40]]})
    blocker = low["targets"][0].get("blocker") or {}
    check(not low["targets"][0]["reachable"] and blocker.get("kind") == "step_too_high"
          and blocker["details"]["rise"] == 8, "check_walkable: steps too high for step_height 4", None, low)
    tall = mcp.call("check_walkable", hull={"height": 100}, **{"from": [512, 256, 0], "to": [[1024, 256, 0]]})
    blocker = tall["targets"][0].get("blocker") or {}
    check(blocker.get("kind") == "headroom" and blocker["details"]["clearance"] == 96,
          "check_walkable: a 100 unit hull does not fit through the 96 unit door", None, tall)
    check(mcp.call_err("check_walkable", **{"from": [776, 100, 48], "to": [[512, 256, 0]]}) == "invalid_argument",
          "check_walkable: from inside a wall")
    # out of the annex to +y, then along -x to a second door in the hall's +x wall
    annex_door = mcp.call("create_opening", wall=annex["group"], side="+y", offset=-64, width=64, height=96)
    hall_door = mcp.call("create_opening", wall=room["group"], side="+x", offset=208, width=64, height=96)
    turn = mcp.call("create_corridor", **{"from": annex_door["opening"], "to": hall_door["opening"]})
    check(turn["shape"] == "l_shaped" and turn["interior_bounds"] == [
        {"min": [928, 400, 0], "max": [992, 432, 96]},
        {"min": [928, 432, 0], "max": [992, 496, 96]},
        {"min": [784, 432, 0], "max": [928, 496, 96]}], "create_corridor: L-shaped", None, turn)
    check(mcp.call("check_brush_overlaps")["pairs"] == [], "L-shaped corridor does not overlap")
    looped = mcp.call("check_leaks", start_points=[[512, 256, 128], [1024, 256, 64]])
    check(looped["sealed"], "check_leaks: still sealed with the L-shaped corridor", None, looped)

    steps.step("checkpoint_revert")
    reverted = mcp.call("checkpoint_revert", name="before_helpers")
    check(reverted["identical"] is True and len(reverted["undone"]) == 14, "checkpoint_revert", None, reverted)
    diff = mcp.call("checkpoint_diff", **{"from": "before_helpers"})
    check(diff["content_identical"] and not diff["added"] and not diff["removed"] and not diff["modified"]
          and not diff["reidentified"], "checkpoint_diff after revert is empty", None, diff)
    check(mcp.call("get_map_stats")["content_digest"] == digest, "revert: digest")


def keep_screenshot(mcp, name, png_bytes):
    if mcp.screenshot_dir:
        with open(os.path.join(mcp.screenshot_dir, name), "wb") as f:
            f.write(png_bytes)


def run_view_checks(mcp, steps, tmpdir):
    box_id = mcp.call("find_nodes", types=["brush"])["ids"][0]

    steps.step("get_camera / set_camera")
    camera = mcp.call("get_camera")
    check(len(camera["position"]) == 3 and camera["fov"] > 0, "get_camera", None, camera)
    moved = mcp.call("set_camera", position=[-200, -200, 200], look_at=[32, 32, 32])
    check(moved["position"] == [-200, -200, 200], "set_camera: position", None, moved)
    check(mcp.call_err("set_camera", look_at=[0, 0, 0], direction=[1, 0, 0]) == "invalid_argument",
          "set_camera: two modes")
    check(mcp.call_err("set_camera", direction=[0, 0, 1]) == "invalid_argument",
          "set_camera: direction parallel to up")
    framed = mcp.call("set_camera", frame={"min": [0, 0, 0], "max": [64, 64, 64]})
    check(framed["position"][2] > 64, "set_camera: frame from above", None, framed)

    steps.step("screenshot iso")
    before = mcp.call("get_camera")
    iso_result = mcp.call_full("screenshot", view="iso", frame_ids=[box_id], max_width=640)
    structured = iso_result["structuredContent"]
    check("_images" not in structured, "image data must not be in structuredContent")
    check(structured["width"] <= 640 and structured["view"] == "iso", "screenshot: size/view", None, structured)
    iso_png = image_from_result(iso_result)
    keep_screenshot(mcp, "iso.png", iso_png)
    iso = decode_png(iso_png)
    check(iso[0] == structured["width"] and iso[1] == structured["height"], "screenshot: PNG size")
    colors = distinct_colors(iso)
    check(len(colors) >= 4, f"screenshot: image is nearly uniform ({len(colors)} colors)")
    after = mcp.call("get_camera")
    check(same_camera(after, before), "screenshot: camera restored", before, after)

    steps.step("screenshot top differs from iso")
    top_result = mcp.call_full("screenshot", view="top", frame_ids=[box_id], max_width=640)
    keep_screenshot(mcp, "top.png", image_from_result(top_result))
    top = decode_png(image_from_result(top_result))
    check(len(distinct_colors(top)) >= 4, "top screenshot is nearly uniform")
    check(top[3] != iso[3], "top and iso screenshots are identical")

    steps.step("screenshot 3d, window and save_path")
    shot_path = os.path.join(tmpdir, "shot.png")
    saved = mcp.call_full("screenshot", save_path=shot_path)
    check(saved["structuredContent"]["saved_path"].endswith("shot.png"), "screenshot: saved_path")
    with open(shot_path, "rb") as f:
        check(f.read() == image_from_result(saved), "screenshot: saved file equals the returned image")
    window = mcp.call_full("screenshot", view="window", max_width=800)
    keep_screenshot(mcp, "window.png", image_from_result(window))
    check(decode_png(image_from_result(window))[0] <= 800, "window screenshot width")
    check(mcp.call_err("screenshot", save_path="relative.png") == "invalid_argument", "screenshot: relative path")
    check(mcp.call_err("screenshot", frame_ids=["B999999"]) == "unknown_node", "screenshot: unknown node")

    steps.step("screenshot_set")
    shot_set = mcp.call_full("screenshot_set", views=["iso", "top", "front"], frame_ids=[box_id])
    shots = shot_set["structuredContent"]["shots"]
    check([s["view"] for s in shots] == ["iso", "top", "front"], "screenshot_set: views", None, shots)
    for index in range(3):
        png = decode_png(image_from_result(shot_set, index))
        check(len(distinct_colors(png)) >= 4, f"screenshot_set: image {index} is nearly uniform")
    after = mcp.call("get_camera")
    check(same_camera(after, before), "screenshot_set: camera restored", before, after)

    steps.step("focus_nodes")
    focused = mcp.call("focus_nodes", ids=[box_id])
    check(focused["ids"] == [box_id], "focus_nodes: ids", None, focused)
    check(mcp.call("get_selection")["nodes"] == [box_id], "focus_nodes keeps the selection")
    mcp.call("select_nodes", mode="clear")


def gray_variance(png, region=0.5):
    """Variance of the gray value in the central part of the image (region = fraction of each side)."""
    width, height, channels, rows = png
    x0, x1 = int(width * (1 - region) / 2), int(width * (1 + region) / 2)
    y0, y1 = int(height * (1 - region) / 2), int(height * (1 + region) / 2)
    values = []
    for row in rows[y0:y1]:
        for x in range(x0, x1):
            r, g, b = row[x * channels:x * channels + 3]
            values.append((299 * r + 587 * g + 114 * b) / 1000)
    mean = sum(values) / len(values)
    return sum((v - mean) ** 2 for v in values) / len(values)


def user_visible_state(mcp):
    """Everything a cutaway must leave unchanged: map text, history, selection, visibility of every node."""
    info = mcp.call("get_map_info")
    ids = mcp.call("find_nodes", limit=5000)["ids"]
    nodes = mcp.call("get_nodes", ids=ids)["nodes"] if ids else []
    return {
        "text": mcp.call("export_map_text")["text"],
        "info": {key: info.get(key) for key in (
            "modified", "modification_count", "undo_label", "redo_label", "can_undo", "can_redo",
            "current_layer", "current_group", "current_material", "selection")},
        "visible": [(n["id"], n.get("visible"), n.get("hidden")) for n in nodes],
    }


def run_cutaway_checks(mcp, steps):
    steps.step("cutaway screenshots of a sealed room")
    if "cutaway" not in json.dumps([t for t in mcp.request("tools/list")["tools"] if t["name"] == "screenshot"]):
        print("  (screenshot has no cutaway; skipped)", flush=True)
        return
    room = mcp.call("create_room", inner_bounds={"min": [2048, 2048, 0], "max": [2560, 2432, 192]},
                    group_name="sealed room", floor_material="floor", wall_material="wall",
                    ceiling_material="ceiling")
    inside = mcp.call("create_box_brush", bounds={"min": [2240, 2176, 0], "max": [2368, 2304, 96]},
                      material="crate")["id"]
    light = mcp.call("create_entity", classname="light", origin=[2144, 2144, 64])["id"]
    group = room["group"]
    leaks = mcp.call("check_leaks", start_points=[[2144, 2144, 64]])
    check(leaks["sealed"], "cutaway room is sealed", None, leaks)

    before = user_visible_state(mcp)
    closed = mcp.call_full("screenshot", view="top", frame_ids=[group], max_width=640)
    opened = mcp.call_full("screenshot", view="top", frame_ids=[group], max_width=640, cutaway={"auto": True})
    cut = opened["structuredContent"]["cutaway"]
    check(cut["hidden_count"] == 1 and cut["hidden_ids"] == [room["ceiling"]], "cutaway auto hides the ceiling",
          None, cut)
    closed_png, opened_png = image_from_result(closed), image_from_result(opened)
    keep_screenshot(mcp, "cutaway-closed-top.png", closed_png)
    keep_screenshot(mcp, "cutaway-auto-top.png", opened_png)
    check(closed_png != opened_png, "cutaway image differs from the closed one")
    closed_variance = gray_variance(decode_png(closed_png))
    opened_variance = gray_variance(decode_png(opened_png))
    check(opened_variance > closed_variance * 2 + 10,
          f"cutaway shows the interior (variance {opened_variance:.1f} vs closed {closed_variance:.1f})")
    check(user_visible_state(mcp) == before, "cutaway leaves the document state unchanged")

    steps.step("cutaway above_z, hide_ids, clip_bounds and screenshot_set")
    iso = mcp.call_full("screenshot", view="iso", frame_ids=[group], max_width=640, cutaway={"above_z": 150})
    check(iso["structuredContent"]["cutaway"]["hidden_ids"] == [room["ceiling"]], "cutaway above_z", None,
          iso["structuredContent"])
    keep_screenshot(mcp, "cutaway-above-z-iso.png", image_from_result(iso))
    # the selection is kept and selected nodes are hidden too
    mcp.call("focus_nodes", ids=[group])
    selected = user_visible_state(mcp)
    hidden = mcp.call_full("screenshot", view="top", frame_ids=[group], max_width=640,
                           cutaway={"hide_ids": ["@selection"]})
    check(hidden["structuredContent"]["cutaway"]["hidden_count"] == 6, "cutaway hide_ids @selection", None,
          hidden["structuredContent"])
    check(user_visible_state(mcp) == selected, "cutaway keeps the selection")
    mcp.call("select_nodes", mode="clear")
    # focus_nodes and select_nodes added undo steps
    before = user_visible_state(mcp)
    shot_set = mcp.call_full("screenshot_set", views=["top", "iso"], frame_ids=[group], max_width=640,
                             cutaway={"auto": True, "clip_bounds": room["inner_bounds"]})
    cut = shot_set["structuredContent"]["cutaway"]
    check(room["ceiling"] in cut["hidden_ids"] and inside not in cut["hidden_ids"]
          and light not in cut["hidden_ids"], "screenshot_set cutaway", None, cut)
    for index, name in enumerate(["top", "iso"]):
        keep_screenshot(mcp, f"cutaway-set-{name}.png", image_from_result(shot_set, index))
    check(image_from_result(shot_set, 0) != closed_png, "screenshot_set cutaway differs from the closed view")
    check(mcp.call_err("screenshot", cutaway={}) == "invalid_argument", "cutaway: empty")
    check(mcp.call_err("screenshot", cutaway={"hide_ids": ["world"]}) == "wrong_node_type", "cutaway: world")
    after_window = mcp.call_full("screenshot", view="top", frame_ids=[group], max_width=640)
    check(image_from_result(after_window) == closed_png, "the closed view renders as before after the cutaways")
    check(user_visible_state(mcp) == before, "cutaways leave the document state unchanged")


def run_shutdown_checks(mcp, steps, map_path):
    steps.step("close_map refuses unsaved changes")
    mcp.call("save_map")
    with open(map_path, "rb") as f:
        saved = f.read()
    saved_brushes = mcp.call("get_map_stats")["counts"]["world_brushes"]
    mcp.call("create_box_brush", bounds={"min": [128, 0, 0], "max": [192, 64, 64]})
    check(mcp.call_err("close_map") == "unsaved_changes", "close_map: unsaved_changes")
    check(mcp.call_err("close_map", discard_changes=False) == "unsaved_changes",
          "close_map: discard_changes false")
    check(len(mcp.call("list_documents")["documents"]) == 1, "map still open")

    steps.step("close_map discards unsaved changes")
    # A save prompt would block the GUI thread, so this call would time out
    closed = mcp.call("close_map", discard_changes=True)
    check(closed["closed"] and closed["remaining_documents"] == 0, "close_map: discard", None, closed)
    check(mcp.call("list_documents")["documents"] == [], "no documents left")
    check(mcp.request("ping") == {}, "ping after close")
    with open(map_path, "rb") as f:
        check(f.read() == saved, "close_map: discard does not touch the file")

    steps.step("discarded changes are gone after reopening")
    mcp.call("open_map", path=map_path)
    stats = mcp.call("get_map_stats")
    check(stats["counts"]["world_brushes"] == saved_brushes, "reopened: brush count", None, stats["counts"])
    mcp.call("create_box_brush", bounds={"min": [128, 0, 0], "max": [192, 64, 64]})
    check(mcp.call("close_map", discard_changes=True)["closed"], "close_map: discard after reopen")
    check(mcp.call("list_documents")["documents"] == [], "no documents left after reopen")
    check(mcp.request("ping") == {}, "ping after last close")


def print_log_tail(tmpdir, lines=40):
    try:
        with open(os.path.join(tmpdir, "trenchbroom.log"), errors="replace") as f:
            tail = f.read().splitlines()[-lines:]
        print("--- TrenchBroom output (tail) ---\n" + "\n".join(tail), file=sys.stderr)
    except OSError:
        pass


class PortableApp:
    """Runs TrenchBroom in portable mode with --mcp-port and a fresh config directory.

    The portable config directory next to the binary is moved aside by start() and
    restored by stop(). TrenchBroom's output goes to <tmpdir>/trenchbroom.log. Also used
    by the scripts in mcp_eval/.
    """

    def __init__(self, app, port=0, keep=False, tmp_prefix="tb-mcp-e2e-"):
        self.binary = os.path.abspath(resolve_binary(app))
        self.port = port or free_port()
        self.keep = keep
        self.config_dir = os.path.join(os.path.dirname(self.binary), "config")
        self.backup_dir = f"{self.config_dir}.mcp-e2e-backup-{os.getpid()}"
        self.tmpdir = tempfile.mkdtemp(prefix=tmp_prefix)
        self.proc = None
        self.moved_config = False
        self.log_file = None

    def start(self, timeout):
        """Starts TrenchBroom and waits until the MCP server answers. Returns an Mcp client."""
        if not os.path.isfile(self.binary):
            raise TestFailure(f"no such binary: {self.binary}")
        if os.path.exists(self.config_dir):
            os.rename(self.config_dir, self.backup_dir)
            self.moved_config = True
        os.makedirs(self.config_dir)
        with open(os.path.join(self.config_dir, "Preferences.json"), "w") as f:
            json.dump({
                "updater/Ask for auto updates": False,
                "updater/Check for updates automatically": False,
            }, f)

        env = dict(os.environ)
        if sys.platform.startswith("linux") and not env.get("DISPLAY") and not env.get("WAYLAND_DISPLAY"):
            env["QT_QPA_PLATFORM"] = "offscreen"
        self.log_file = open(os.path.join(self.tmpdir, "trenchbroom.log"), "w")
        self.proc = subprocess.Popen(
            [self.binary, "--portable", f"--mcp-port={self.port}"],
            cwd=self.tmpdir, env=env, stdout=self.log_file, stderr=subprocess.STDOUT)
        print(f"started TrenchBroom (pid {self.proc.pid}) with --mcp-port={self.port}", flush=True)

        mcp = Mcp(self.port)
        wait_for_server(mcp, self.proc, timeout)
        return mcp

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def crash_reports(self):
        """Names of crash reports TrenchBroom wrote into its working or config directory."""
        names = []
        for directory in (self.tmpdir, self.config_dir):
            if os.path.isdir(directory):
                names += [name for name in os.listdir(directory) if "crash" in name.lower()]
        return names

    def print_log_tail(self, lines=40):
        print_log_tail(self.tmpdir, lines)

    def stop(self):
        """Terminates TrenchBroom and restores the config. Returns False if it crashed."""
        ok = True
        if self.proc is not None and not self.keep:
            if self.proc.poll() is None:
                self.proc.terminate()
                try:
                    self.proc.wait(10)
                except subprocess.TimeoutExpired:
                    self.proc.kill()
                    self.proc.wait()
            if self.proc.returncode is not None and self.proc.returncode < 0 \
                    and self.proc.returncode != -signal.SIGTERM:
                print(f"FAIL: TrenchBroom ended with signal {-self.proc.returncode}", file=sys.stderr)
                ok = False
        if self.log_file is not None:
            self.log_file.close()
        if not self.keep:
            shutil.rmtree(self.config_dir, ignore_errors=True)
            if self.moved_config:
                os.rename(self.backup_dir, self.config_dir)
            shutil.rmtree(self.tmpdir, ignore_errors=True)
        else:
            print(f"kept TrenchBroom running; temp dir {self.tmpdir}; config {self.config_dir}"
                  + (f" (original config in {self.backup_dir})" if self.moved_config else ""))
        return ok


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--app", required=True, help="TrenchBroom binary or TrenchBroom.app")
    parser.add_argument("--keep", action="store_true", help="keep TrenchBroom running and the temp dir")
    parser.add_argument("--timeout", type=float, default=60)
    parser.add_argument("--port", type=int, default=0, help="MCP port; 0 picks a free one")
    parser.add_argument("--screenshot-dir", help="also save some screenshots to this directory")
    args = parser.parse_args()

    if not os.path.isfile(resolve_binary(args.app)):
        print(f"no such binary: {os.path.abspath(resolve_binary(args.app))}", file=sys.stderr)
        return 2

    app = PortableApp(args.app, args.port, args.keep)
    exit_code = 1
    try:
        mcp = app.start(args.timeout)
        mcp.screenshot_dir = args.screenshot_dir
        tmpdir = app.tmpdir
        steps = Steps(app.proc)
        run_protocol_checks(mcp, steps)
        mcp.tool_names = {t["name"] for t in mcp.request("tools/list")["tools"]}
        map_path = run_scenario(mcp, steps, tmpdir)
        run_helper_checks(mcp, steps)
        run_view_checks(mcp, steps, tmpdir)
        run_cutaway_checks(mcp, steps)
        run_shutdown_checks(mcp, steps, map_path)
        steps.finish()
        mcp.close()

        crash_reports = app.crash_reports()
        check(not crash_reports, f"crash reports were written: {crash_reports}")
        print("PASS: all MCP end-to-end checks passed", flush=True)
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


if __name__ == "__main__":
    sys.exit(main())
