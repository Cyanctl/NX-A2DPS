"""Small stdlib client for the user-supplied Switch MCP HTTP endpoint."""
import argparse
import base64
import json
from pathlib import Path
import http.client
from urllib.parse import urlsplit


class Client:
    def __init__(self, url):
        self.url = url
        self.session = None
        self.counter = 0
        self.request("initialize", {
            "protocolVersion": "2025-06-18", "capabilities": {},
            "clientInfo": {"name": "switch-audio-research", "version": "0.1.0"},
        })
        # SwitchMCPServer 1.0.0 rejects JSON-RPC notifications; tool calls work
        # immediately after initialize on this implementation.

    def request(self, method, params=None, notification=False):
        self.counter += 1
        payload = {"jsonrpc": "2.0", "method": method}
        if not notification:
            payload["id"] = self.counter
        if params is not None:
            payload["params"] = params
        headers = {"Content-Type": "application/json",
                   "Accept": "application/json, text/event-stream"}
        if self.session:
            headers["Mcp-Session-Id"] = self.session
            headers["MCP-Protocol-Version"] = "2025-06-18"
        body = json.dumps(payload, separators=(",", ":")).encode()
        url = urlsplit(self.url)
        if url.scheme != "http":
            raise ValueError("This console client supports local HTTP endpoints only")
        conn = http.client.HTTPConnection(url.hostname, url.port, timeout=30)
        headers.update({"Host": url.netloc, "Content-Length": str(len(body)),
                        "Connection": "close"})
        request = f"POST {url.path or '/'} HTTP/1.1\r\n"
        request += "".join(f"{key}: {value}\r\n" for key, value in headers.items())
        # Send header and body together: this server sometimes parses before a
        # separately transmitted request body arrives.
        conn.connect()
        try:
            conn.send(request.encode() + b"\r\n" + body)
            response = http.client.HTTPResponse(conn.sock)
            response.begin()
            self.session = response.headers.get("Mcp-Session-Id", self.session)
            raw = response.read().decode()
            if response.status >= 400:
                raise RuntimeError(f"MCP HTTP {response.status}: {raw}")
        finally:
            conn.close()
        if not raw.strip():
            return None
        if raw.lstrip().startswith("{"):
            result = json.loads(raw)
        else:
            result = None
            for event in raw.replace("\r\n", "\n").split("\n\n"):
                data = "\n".join(line[5:].lstrip() for line in event.splitlines()
                                 if line.startswith("data:"))
                if data:
                    candidate = json.loads(data)
                    if candidate.get("id") == self.counter:
                        result = candidate
            if result is None:
                raise RuntimeError("MCP response contained no matching result")
        if "error" in result:
            raise RuntimeError(result["error"])
        return result.get("result")

    def call(self, name, arguments=None):
        result = self.request("tools/call", {"name": name, "arguments": arguments or {}})
        if result.get("isError"):
            raise RuntimeError(result)
        return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--url", default="http://192.168.178.74:12345/mcp")
    parser.add_argument("action", choices=["list", "frame", "press"])
    parser.add_argument("--output", default=".research/switch-frame.jpg")
    parser.add_argument("--buttons", type=lambda x: int(x, 0), default=0)
    args = parser.parse_args()
    client = Client(args.url)
    if args.action == "list":
        print(json.dumps(client.request("tools/list"), indent=2))
    elif args.action == "press":
        print(json.dumps(client.call("controller", {"buttons": args.buttons})))
    else:
        result = client.call("cur_frame")
        for block in result.get("content", []):
            if block.get("type") == "image":
                path = Path(args.output)
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(base64.b64decode(block["data"], validate=True))
                print(path.resolve())
                break
        else:
            raise RuntimeError("No image returned: " + str(result)[:500])


if __name__ == "__main__":
    main()
