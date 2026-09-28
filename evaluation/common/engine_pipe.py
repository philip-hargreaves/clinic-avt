"""Client for the engine's named pipe: length-prefixed JSON-RPC, as the shell speaks it.

    engine = Engine([exe, pipe, ...], pipe, log_path)
    engine.wait_up()
    result = engine.request("session/list")
    engine.close()

close() asks engine/exit and waits; it never kills, because a process stopped mid-GPU can wedge
the graphics driver.
"""

import collections
import ctypes
import ctypes.wintypes
import json
import msvcrt
import struct
import subprocess
import time


class EngineDied(RuntimeError):
    pass


class Engine:
    # One synchronous pipe handle serialises reads and writes, so a blocking read on another
    # thread would stall every write. PeekNamedPipe says how much is waiting and only that much
    # is read.
    def __init__(self, command, pipe_name, log_path, cwd=None):
        self.log_path = log_path
        self.log_offset = 0
        self.notifications = collections.deque()  # (arrival time, message)
        self.replies = {}
        self.buf = b""
        self.dead = False
        self.next_id = 0
        self.launched = time.perf_counter()
        self.stderr = open(log_path, "wb")
        self.proc = subprocess.Popen(command, stderr=self.stderr, stdout=subprocess.DEVNULL, cwd=cwd)
        for _ in range(600):
            if self.proc.poll() is not None:
                raise EngineDied(f"exited {self.proc.returncode} before the pipe appeared")
            try:
                self.f = open("\\\\.\\pipe\\" + pipe_name, "r+b", buffering=0)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise EngineDied("pipe never appeared")
        self.handle = msvcrt.get_osfhandle(self.f.fileno())

    def _available(self):
        avail = ctypes.wintypes.DWORD(0)
        ok = ctypes.windll.kernel32.PeekNamedPipe(
            ctypes.c_void_p(self.handle), None, 0, None, ctypes.byref(avail), None)
        if not ok:
            self.dead = True
            raise EngineDied(f"pipe broke (error {ctypes.GetLastError()})")
        return avail.value

    def _pump(self):
        # True if anything arrived
        avail = self._available()
        if avail == 0:
            if self.proc.poll() is not None:
                self.dead = True
                raise EngineDied(f"engine exited {self.proc.returncode}")
            return False
        chunk = self.f.read(min(avail, 1 << 20))
        if not chunk:
            self.dead = True
            raise EngineDied("pipe closed")
        self.buf += chunk
        t = time.perf_counter()
        while len(self.buf) >= 4:
            (length,) = struct.unpack("<I", self.buf[:4])
            if len(self.buf) < 4 + length:
                break
            msg = json.loads(self.buf[4:4 + length])
            self.buf = self.buf[4 + length:]
            if msg.get("id") is not None and "method" not in msg:
                self.replies[msg["id"]] = msg
            else:
                self.notifications.append((t, msg))
        return True

    def request(self, method, params=None, timeout=30.0):
        if self.dead:
            raise EngineDied("connection lost")
        self.next_id += 1
        rid = self.next_id
        msg = {"jsonrpc": "2.0", "id": rid, "method": method}
        if params is not None:
            msg["params"] = params
        payload = json.dumps(msg).encode()
        try:
            self.f.write(struct.pack("<I", len(payload)) + payload)
        except OSError as e:
            self.dead = True
            raise EngineDied(f"write failed: {e}")
        deadline = time.perf_counter() + timeout
        while rid not in self.replies:
            if not self._pump():
                if time.perf_counter() >= deadline:
                    raise TimeoutError(f"{method} did not answer in {timeout} s")
                time.sleep(0.005)
        reply = self.replies.pop(rid)
        if "error" in reply:
            raise RuntimeError(f"{method}: {reply['error']}")
        return reply.get("result")

    def next_notification(self, timeout):
        # (arrival time, message), or None on timeout
        deadline = time.perf_counter() + timeout
        while True:
            if self.notifications:
                return self.notifications.popleft()
            if not self._pump():
                if time.perf_counter() >= deadline:
                    return None
                time.sleep(0.005)

    def wait_for(self, methods, timeout):
        # Drops other notifications until one of `methods` arrives; None on timeout
        deadline = time.perf_counter() + timeout
        while time.perf_counter() < deadline:
            while self.notifications:
                _, msg = self.notifications.popleft()
                if msg.get("method") in methods:
                    return msg
            if not self._pump():
                time.sleep(0.02)
        return None

    def wait_up(self, seconds=60):
        # Echo answers as soon as the pipe serves, before any model has loaded
        deadline = time.perf_counter() + seconds
        while True:
            try:
                self.request("engine/echo", {"payload": "up"}, 2)
                return
            except EngineDied:
                raise
            except (TimeoutError, RuntimeError):
                if time.perf_counter() >= deadline:
                    raise TimeoutError(f"engine did not answer echo in {seconds} s")
                time.sleep(0.1)

    def exit_code(self):
        return self.proc.poll()

    def new_log_lines(self):
        # The engine's stderr since the last call
        with open(self.log_path, "rb") as f:
            f.seek(self.log_offset)
            data = f.read()
            self.log_offset = f.tell()
        return data.decode("utf-8", "replace").splitlines()

    def close(self):
        # The engine leaves once asked; a note model still loading finishes first
        try:
            if not self.dead:
                self.request("engine/exit", timeout=10)
        except Exception:
            pass
        try:
            self.f.close()
        except Exception:
            pass
        try:
            self.proc.wait(600)
        except subprocess.TimeoutExpired:
            print(f"engine {self.proc.pid} still running 10 min after close; leaving it", flush=True)
        self.stderr.close()
