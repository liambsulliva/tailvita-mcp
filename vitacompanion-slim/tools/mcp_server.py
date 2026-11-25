import base64
import ftplib
import importlib.util
import io
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import zipfile

SERVER_NAME = "vitacompanion"
SERVER_VERSION = "0.1.0"
PROTOCOL_VERSION = "2025-06-18"
# VITA_HOST pins one address. Without it every call picks the route: the Vita on this
# machine's local network first, the tailnet only when it isn't there.
FORCED_HOST = os.environ.get("VITA_HOST", "")
LAN_HOST = os.environ.get("VITA_LAN_HOST", "10.0.0.151")
TAILNET_HOST = os.environ.get("VITA_TAILNET_HOST", "100.96.148.83")
FTP_PORT = int(os.environ.get("VITA_FTP_PORT", "1337"))
COMMAND_PORT = int(os.environ.get("VITA_COMMAND_PORT", "1338"))
DEFAULT_SRC_VITA = os.environ.get("VITA_SRC_VITA", "")
PYTHON = os.environ.get("VITA_LOOP_PYTHON", sys.executable)
VITASDK = os.environ.get("VITASDK", "/usr/local/vitasdk")
INSTALL_STAGE = "ux0:/data/vitacompanion_mcp/install"
REPLY_SECONDS = 3.0
PROMOTE_SECONDS = 90.0
LOOP_SECONDS = 1800
BUILD_SECONDS = 900
OUTPUT_TAIL = 12000
LIST_LIMIT = 500
ROUTE_SECONDS = 30
SWEEP_AGAIN_SECONDS = 600


def load_finder():
    # python -I leaves this folder off the import path
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "vita_find.py")
    spec = importlib.util.spec_from_file_location("vita_find", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    module.CMD_PORT = COMMAND_PORT
    return module


class Route:
    """Where the Vita is: (name, host), kept for ROUTE_SECONDS. A network searched without
    finding it is searched again after SWEEP_AGAIN_SECONDS, or by vita_status."""

    def __init__(self):
        self.finder = load_finder()
        self.known = [LAN_HOST]
        self.swept = {}
        self.picked = None
        self.until = 0.0

    def pick(self, fresh=False):
        if FORCED_HOST:
            return "forced", FORCED_HOST
        now = time.monotonic()
        if self.picked and now < self.until and not fresh:
            return self.picked
        skip = [] if fresh else [net for net, at in self.swept.items() if now - at < SWEEP_AGAIN_SECONDS]
        found = self.finder.find(self.known, skip)
        for net in found["swept"]:
            self.swept[net] = time.monotonic()
        if found["host"]:
            self.swept.pop(found["network"], None)
            if found["host"] not in self.known:
                self.known.insert(0, found["host"])
            self.picked = "lan", found["host"]
        else:
            self.picked = "tailnet", TAILNET_HOST
        self.until = time.monotonic() + ROUTE_SECONDS
        return self.picked


ROUTE = Route()


def vita_host():
    return ROUTE.pick()[1]


def refuse_during_usb(name):
    """The card is on the Mac in USB mode while the Vita still has it mounted."""
    why = ROUTE.finder.usb() if name.startswith("vita_") and name not in NO_CARD else ""
    if why:
        raise RuntimeError(why + ". The network stays closed until `vita usb end` has run")


NO_CARD = ("vita_status", "vita_build", "vita_show_image")


def device_command(text, reply_seconds=REPLY_SECONDS):
    with socket.create_connection((vita_host(), COMMAND_PORT), timeout=10) as connection:
        connection.sendall(text.strip().encode("ascii") + b"\n")
        connection.settimeout(reply_seconds)
        reply = b""
        try:
            while True:
                chunk = connection.recv(4096)
                if not chunk:
                    break
                reply += chunk
        except socket.timeout:
            pass
    return reply.decode("ascii", "replace").strip()


def remote(path):
    path = path.strip()
    return path if path.startswith("/") else "/" + path


def open_ftp():
    connection = ftplib.FTP()
    connection.connect(vita_host(), FTP_PORT, timeout=60)
    connection.login("anonymous", "anonymous")
    connection.voidcmd("TYPE I")
    return connection


def make_folders(connection, path):
    built = ""
    for part in remote(path).strip("/").split("/"):
        built = built + "/" + part
        if part.endswith(":"):
            continue
        try:
            connection.mkd(built)
        except ftplib.error_perm:
            pass


def delete_if_present(connection, path):
    try:
        connection.delete(path)
    except ftplib.error_perm:
        pass


def read_back(connection, path):
    output = io.BytesIO()
    connection.retrbinary("RETR " + path, output.write)
    return output.getvalue()


def put_verified(connection, data, path):
    partial = path + ".new"
    connection.storbinary("STOR " + partial, io.BytesIO(data))
    if read_back(connection, partial) != data:
        delete_if_present(connection, partial)
        raise RuntimeError("read-back differs for " + partial + "; left nothing in place")
    delete_if_present(connection, path)
    connection.rename(partial, path)


def upload_tree(connection, local_folder, remote_folder):
    count = 0
    for folder, _, names in os.walk(local_folder):
        relative = os.path.relpath(folder, local_folder)
        target = remote_folder if relative == "." else remote_folder + "/" + relative.replace(os.sep, "/")
        make_folders(connection, target)
        for name in sorted(names):
            with open(os.path.join(folder, name), "rb") as source:
                put_verified(connection, source.read(), target + "/" + name)
            count += 1
    return count


def ftp_list(path):
    connection = open_ftp()
    try:
        lines = []
        connection.retrlines("LIST " + remote(path), lines.append)
    finally:
        connection.close()
    return "\n".join(lines[:LIST_LIMIT]) + ("\n... %d more" % (len(lines) - LIST_LIMIT) if len(lines) > LIST_LIMIT else "")


def ftp_upload(local_path, remote_path):
    local_path = os.path.expanduser(local_path)
    connection = open_ftp()
    try:
        if os.path.isdir(local_path):
            return "uploaded %d files to %s" % (upload_tree(connection, local_path, remote(remote_path).rstrip("/")), remote_path)
        make_folders(connection, remote(remote_path).rsplit("/", 1)[0])
        with open(local_path, "rb") as source:
            data = source.read()
        put_verified(connection, data, remote(remote_path))
    finally:
        connection.close()
    return "uploaded %d bytes to %s, read back and matched" % (len(data), remote_path)


def ftp_download(remote_path, local_path):
    local_path = os.path.expanduser(local_path)
    connection = open_ftp()
    try:
        data = read_back(connection, remote(remote_path))
    finally:
        connection.close()
    os.makedirs(os.path.dirname(os.path.abspath(local_path)), exist_ok=True)
    with open(local_path, "wb") as output:
        output.write(data)
    return "saved %d bytes to %s" % (len(data), local_path)


def ftp_delete(remote_path):
    connection = open_ftp()
    try:
        connection.delete(remote(remote_path))
    finally:
        connection.close()
    return "deleted " + remote_path


def install_vpk(vpk_path):
    vpk_path = os.path.expanduser(vpk_path)
    work = tempfile.mkdtemp(prefix="vcmcp_")
    try:
        with zipfile.ZipFile(vpk_path) as archive:
            for member in archive.namelist():
                if member.startswith("/") or ".." in member.split("/"):
                    raise RuntimeError("unsafe path in package: " + member)
            archive.extractall(work)
        if not os.path.isfile(os.path.join(work, "sce_sys", "param.sfo")):
            raise RuntimeError("package has no sce_sys/param.sfo")
        connection = open_ftp()
        try:
            count = upload_tree(connection, work, "/" + INSTALL_STAGE)
        finally:
            connection.close()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    reply = device_command("promote " + INSTALL_STAGE.replace(":/", ":", 1), PROMOTE_SECONDS)
    return "staged %d files; promote: %s" % (count, reply)


def src_vita_of(arguments):
    folder = os.path.expanduser(arguments.get("src_vita") or DEFAULT_SRC_VITA)
    if not folder or not os.path.isfile(os.path.join(folder, "tools", "vita_loop.py")):
        raise RuntimeError("src_vita must be a srcVita folder holding tools/vita_loop.py")
    return folder


def run(command, folder, seconds, environment=None):
    merged = dict(os.environ, VITA_HOST=vita_host())
    merged.update(environment or {})
    result = subprocess.run(command, cwd=folder, env=merged, capture_output=True, text=True, timeout=seconds)
    output = (result.stdout + result.stderr)[-OUTPUT_TAIL:]
    return "exit %d\n%s" % (result.returncode, output)


def loop_action(arguments):
    folder = src_vita_of(arguments)
    command = [PYTHON, "-I", os.path.join("tools", "vita_loop.py"), arguments["action"]] + [str(value) for value in arguments.get("arguments", [])]
    return run(command, folder, LOOP_SECONDS)


def build(arguments):
    folder = src_vita_of(arguments)
    command = ["make", "-j8"] + ["%s=%s" % (key, value) for key, value in sorted(arguments.get("variables", {}).items())]
    environment = {"VITASDK": VITASDK, "PATH": os.path.join(VITASDK, "bin") + os.pathsep + os.environ.get("PATH", "")}
    return run(command, folder, BUILD_SECONDS, environment)


def status(_):
    name, host = ROUTE.pick(fresh=True)
    lines = ["route %s host %s ftp %d command %d" % (name, host, FTP_PORT, COMMAND_PORT)]
    try:
        lines.append("companion: " + (device_command("version") or "no reply"))
    except OSError as error:
        lines.append("command port unreachable: %s" % error)
    try:
        connection = open_ftp()
        connection.close()
        lines.append("ftp: answering")
    except (OSError, ftplib.Error) as error:
        lines.append("ftp unreachable: %s" % error)
    if ROUTE.finder.usb():
        lines.append("usb: " + ROUTE.finder.usb())
    return "\n".join(lines)


def image_of(arguments):
    path = os.path.expanduser(arguments["path"])
    work = tempfile.mkdtemp(prefix="vcmcp_")
    try:
        png = os.path.join(work, "shot.png")
        subprocess.run(["magick", path, "-resize", "%d>" % int(arguments.get("width", 960)), png], check=True, capture_output=True)
        with open(png, "rb") as source:
            return {"type": "image", "data": base64.b64encode(source.read()).decode("ascii"), "mimeType": "image/png"}
    finally:
        shutil.rmtree(work, ignore_errors=True)


def text_schema(properties, required):
    return {"type": "object", "properties": properties, "required": required, "additionalProperties": False}


SRC_VITA_PROPERTY = {"type": "string", "description": "srcVita folder to act on (a checkout or worktree). Defaults to VITA_SRC_VITA."}

TOOLS = [
    ("vita_status", "Report the Vita's address and whether Vita Companion's command port and FTP answer.", text_schema({}, []), status),
    ("vita_command", "Send one line to the Vita Companion command port and return its reply. Commands: help, version, launch <TITLEID>, quit <TITLEID>|all, press/release <button|stick|touch>, wait <n>ms, screen on|off, nosleep on|off|status, promote <dir>, reboot. Chain with ';'.", text_schema({"text": {"type": "string"}, "reply_seconds": {"type": "number", "description": "how long to collect the reply, default 3"}}, ["text"]), lambda a: device_command(a["text"], float(a.get("reply_seconds", REPLY_SECONDS))) or "(no reply)"),
    ("vita_ftp_list", "List a folder on the Vita over FTP, for example ux0:/data/.", text_schema({"path": {"type": "string"}}, ["path"]), lambda a: ftp_list(a["path"])),
    ("vita_ftp_upload", "Upload a local file or folder to the Vita over FTP. Each file goes to <path>.new, is read back and compared, then replaces the target.", text_schema({"local_path": {"type": "string"}, "remote_path": {"type": "string"}}, ["local_path", "remote_path"]), lambda a: ftp_upload(a["local_path"], a["remote_path"])),
    ("vita_ftp_download", "Download one file from the Vita over FTP to a local path.", text_schema({"remote_path": {"type": "string"}, "local_path": {"type": "string"}}, ["remote_path", "local_path"]), lambda a: ftp_download(a["remote_path"], a["local_path"])),
    ("vita_ftp_delete", "Delete one file on the Vita over FTP.", text_schema({"remote_path": {"type": "string"}}, ["remote_path"]), lambda a: ftp_delete(a["remote_path"])),
    ("vita_install_vpk", "Install a .vpk: unpack it locally, upload it to a staging folder with verified writes, then ask Vita Companion to promote it.", text_schema({"vpk_path": {"type": "string"}}, ["vpk_path"]), lambda a: install_vpk(a["vpk_path"])),
    ("vita_build", "Run make -j8 in a srcVita folder with the VitaSDK on the path. variables become NAME=value make arguments, such as DEBUG=1.", text_schema({"src_vita": SRC_VITA_PROPERTY, "variables": {"type": "object", "additionalProperties": {"type": "string"}}}, []), build),
    ("vita_loop", "Run a srcVita/tools/vita_loop.py action against the Vita: first-install, upload-eboot, upload-assets <folder>, launch, quit, log, capture <name> [--output dir], pull <name>, burst --output dir, device-command <text>, app-command <text>, wait <text> [--seconds n].", text_schema({"src_vita": SRC_VITA_PROPERTY, "action": {"type": "string"}, "arguments": {"type": "array", "items": {"type": "string"}}}, ["action"]), loop_action),
    ("vita_show_image", "Return a local image (such as a capture BMP pulled by vita_loop) as a PNG for viewing.", text_schema({"path": {"type": "string"}, "width": {"type": "integer"}}, ["path"]), image_of),
]
HANDLERS = {name: handler for name, _, _, handler in TOOLS}


def reply(message_id, result=None, error=None):
    message = {"jsonrpc": "2.0", "id": message_id}
    if error is not None:
        message["error"] = error
    else:
        message["result"] = result
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()


def call_tool(params):
    name = params.get("name")
    if name not in HANDLERS:
        return {"content": [{"type": "text", "text": "unknown tool: %s" % name}], "isError": True}
    try:
        refuse_during_usb(name)
        outcome = HANDLERS[name](params.get("arguments") or {})
    except Exception as error:
        return {"content": [{"type": "text", "text": "%s: %s" % (type(error).__name__, error)}], "isError": True}
    if isinstance(outcome, dict):
        return {"content": [outcome]}
    return {"content": [{"type": "text", "text": outcome}], "isError": outcome.startswith("exit ") and not outcome.startswith("exit 0")}


def handle(message):
    method = message.get("method")
    message_id = message.get("id")
    if message_id is None:
        return
    if method == "initialize":
        requested = (message.get("params") or {}).get("protocolVersion", PROTOCOL_VERSION)
        reply(message_id, {"protocolVersion": requested, "capabilities": {"tools": {}}, "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION}})
    elif method == "tools/list":
        reply(message_id, {"tools": [{"name": name, "description": description, "inputSchema": schema} for name, description, schema, _ in TOOLS]})
    elif method == "tools/call":
        reply(message_id, call_tool(message.get("params") or {}))
    elif method == "ping":
        reply(message_id, {})
    else:
        reply(message_id, error={"code": -32601, "message": "method not found: %s" % method})


def main():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            continue
        handle(message)


if __name__ == "__main__":
    main()
