# MCP Library

A library that implements the server side of the
[Model Context Protocol](https://modelcontextprotocol.io) (MCP), which lets AI agents call
tools offered by an application. The library only depends on Qt Core, Qt Network and
KdLib. It doesn't know anything about TrenchBroom; the editor-specific tools live in
TbUiLib.

## Overview

The library is split into layers that can be tested on their own:

- `Http.h`: an incremental HTTP/1.x request parser with size limits, and HTTP responses.
- `JsonRpc.h`: classification of JSON-RPC 2.0 messages and construction of results and
  errors.
- `JsonSchema.h`: a validator for the subset of JSON Schema used by tool input schemas.
  The schemas are the single source of truth for the shape of tool arguments.
- `Tool.h`: tool specifications, the tool registry and the conversion of tool results to
  MCP `CallToolResult` objects.
- `Server.h`: the MCP lifecycle (`initialize`, `ping`, `tools/list`, `tools/call`) and
  sessions on top of the
  [Streamable HTTP](https://modelcontextprotocol.io/specification/2025-06-18/basic/transports#streamable-http)
  transport. The server maps an `HttpRequest` to an `HttpResponse` and does no I/O itself.
- `HttpTransport.h`: a small HTTP/1.1 server on `QTcpServer` that listens on 127.0.0.1
  only and passes each request to a handler such as `Server::handle`. It supports
  keep-alive and pipelining, enforces the parser's size limits, limits the number of
  connections and closes idle connections.

The server supports the protocol versions 2025-06-18 and 2025-03-26. It only sends JSON
responses (no server-sent events), never sends requests to the client and does not
support JSON-RPC batches.

Everything runs on the thread that calls `Server::handle`. Tool handlers are called
synchronously. A tool call that arrives while another one is still running (e.g. because a
handler spun a nested event loop) gets the tool error `editor_busy`.

## Usage

```cpp
auto tools = tb::mcp::ToolRegistry{};
tools.add(tb::mcp::ToolSpec{
  .name = "echo",
  .title = "Echo",
  .description = "Returns its text argument.",
  .inputSchema = QJsonObject{
    {"type", "object"},
    {"properties", QJsonObject{{"text", QJsonObject{{"type", "string"}}}}},
    {"required", QJsonArray{"text"}},
    {"additionalProperties", false},
  },
  .annotations = {.readOnly = true, .idempotent = true},
  .handler = [](const QJsonObject& arguments) -> tb::mcp::ToolResult {
    return tb::mcp::ToolOutput{.structured = {{"text", arguments["text"]}}};
  },
});

auto server = tb::mcp::Server{tb::mcp::ServerInfo{.version = "1.0"}, tools};
auto transport = tb::mcp::HttpTransport{
  [&](const tb::mcp::HttpRequest& request, const quint16 localPort) {
    return server.handle(request, localPort);
  }};
if (const auto result = transport.listen(6767); result.is_error())
{
  // the port is in use
}
```

The transport must not be destroyed from within the handler; use `deleteLater` instead.

Tool handlers receive arguments that have not been validated yet; callers are expected to
run `tb::mcp::validate` against the input schema first.

## Security

The server is meant to be bound to the loopback interface only. It rejects requests whose
`Host` header does not name a loopback address with the local port (DNS rebinding) and
requests with an `Origin` header that is not a loopback http(s) origin. It never sends
CORS headers.

There is no authentication: while the server is running, any local process can call its
tools. Applications should therefore only start the server when the user asks for it.
