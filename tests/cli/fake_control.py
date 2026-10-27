#!/usr/bin/env python3
# SPDX-License-Identifier: CC0-1.0
#
# Stands in for a running scva-daemon's control socket: accepts one client,
# records what it sent, answers with a fixed reply, and exits.
#
#   fake_control.py SOCKET RECORD REPLY
import os
import socket
import sys

path, record, reply = sys.argv[1], sys.argv[2], sys.argv[3]
if os.path.exists(path):
    os.unlink(path)
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
srv.bind(path)
srv.listen(1)
srv.settimeout(20)
conn, _ = srv.accept()
data = b""
while True:
    chunk = conn.recv(4096)
    if not chunk:
        break
    data += chunk
with open(record, "wb") as f:
    f.write(data)
conn.sendall(reply.encode())
conn.close()
srv.close()
os.unlink(path)
