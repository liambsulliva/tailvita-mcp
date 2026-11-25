#!/usr/bin/env python3
"""Find the Vita on the local network of the machine this runs on.

bin/vita sends this script to the Mac over SSH and runs it there, so nothing is installed
on the Mac. It knows the Vita by VitaCompanion's reply to `version` on the command port,
never by address alone: the same 192.168.x.y is another device on another network.

Only networks this machine is directly on are tried: private (RFC 1918) IPv4 on Wi-Fi,
Ethernet or a bridge (Internet Sharing). VPN interfaces, the tailnet among them, are not.
Known addresses come first. If none answers, every address of a network is tried once,
for networks of at most 1024 addresses.

  vita-find ['{"candidates": ["10.0.0.151"], "skip_sweep": ["192.168.4.0/22"], "hold": true}']

Prints one JSON line: host and version (null when not found), network (the one it was
found on), networks (tried) and swept (searched address by address). With "hold" it then
waits for stdin to close, which keeps the SSH connection's port forwarding open.
Python 3.9 or later, stdlib only.

tailvita-mcp carries a copy as vitacompanion-slim/tools/vita_find.py, for the MCP server."""
import concurrent.futures
import ipaddress
import json
import os
import re
import socket
import subprocess
import sys

CMD_PORT = int(os.environ.get("VITA_CMD_PORT", "1338"))
CONNECT_SECONDS = 1.5
REPLY_SECONDS = 4.0
WORKERS = 128  # macOS allows 256 open files per process over SSH
MAX_ADDRESSES = 1024
PRIVATE = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]


def parse_ifconfig(text):
    """[IPv4Interface] of Wi-Fi, Ethernet and bridge interfaces in macOS ifconfig output."""
    out, name = [], ""
    for line in text.splitlines():
        head = re.match(r"(\w+):", line)
        if head:
            name = head.group(1)
            continue
        m = re.match(r"\s+inet (\S+) netmask 0x([0-9a-f]{8})", line)
        if m and re.fullmatch(r"(en|bridge)\d+", name):
            out.append(ipaddress.ip_interface(f"{m.group(1)}/{bin(int(m.group(2), 16)).count('1')}"))
    return out


def networks():
    """Directly attached private networks, without duplicates."""
    if os.environ.get("VITA_FIND_NETS"):  # for tests
        return [ipaddress.ip_network(n) for n in os.environ["VITA_FIND_NETS"].split()]
    text = subprocess.run(["/sbin/ifconfig"], capture_output=True, text=True, timeout=10).stdout
    found = [i.network for i in parse_ifconfig(text) if any(i.ip in p for p in PRIVATE)]
    return list(dict.fromkeys(found))


def companion(host):
    """VitaCompanion's version line if it answers at host, else None."""
    try:
        with socket.create_connection((host, CMD_PORT), timeout=CONNECT_SECONDS) as conn:
            conn.settimeout(REPLY_SECONDS)
            conn.sendall(b"version\n")
            reply = conn.recv(256).decode("ascii", "replace").strip()
    except OSError:
        return None
    return reply if reply.lower().startswith("vitacompanion") else None


def find(candidates=(), skip_sweep=()):
    nets = networks()
    result = {"host": None, "version": None, "network": None, "networks": [str(n) for n in nets], "swept": []}

    def hit(host, net, version):
        result.update(host=host, network=str(net), version=version)
        return result

    for host in dict.fromkeys(candidates):
        net = next((n for n in nets if ipaddress.ip_address(host) in n), None)
        version = companion(host) if net else None
        if version:
            return hit(host, net, version)
    for net in nets:
        if str(net) in skip_sweep or net.num_addresses > MAX_ADDRESSES:
            continue
        result["swept"].append(str(net))
        pool = concurrent.futures.ThreadPoolExecutor(WORKERS)
        jobs = {pool.submit(companion, str(h)): str(h) for h in net.hosts()}
        try:
            for job in concurrent.futures.as_completed(jobs):
                if job.result():
                    return hit(jobs[job], net, job.result())
        finally:
            pool.shutdown(wait=True, cancel_futures=True)
    return result


def main():
    ask = json.loads(sys.argv[1]) if len(sys.argv) > 1 else {}
    print(json.dumps(find(ask.get("candidates", ()), ask.get("skip_sweep", ()))), flush=True)
    if ask.get("hold"):
        sys.stdin.read()


if __name__ == "__main__":
    main()
