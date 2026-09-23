"""Exercise the production plain host with a deliberately strict module ABI.

The fixture uses mmap/munmap for results, delays initialization, and exposes a
blocking method. These checks caught regressions that ordinary module calls did
not: freeing module memory with free(), publishing before set_context finishes,
and dropping the configured worker limit.
"""

import ctypes
import os
import queue
import resource
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path


def library(directory: Path, stem: str) -> Path:
    for suffix in (".so", ".dylib"):
        candidate = directory / f"lib{stem}{suffix}"
        if candidate.exists():
            return candidate
    raise RuntimeError(f"missing lib{stem} shared library in {directory}")


HOST = Path(sys.argv[1])
FIXTURE = library(Path(sys.argv[2]), "plain_host_fixture")
PROTOCOL = library(Path(sys.argv[3]), "logos_protocol_plain")

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
signal.alarm(40)

api = ctypes.CDLL(str(PROTOCOL))
api.lp_token_save.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
api.lp_client_create.argtypes = [ctypes.c_char_p] * 4
api.lp_client_create.restype = ctypes.c_void_p
api.lp_invoke.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                          ctypes.c_int, ctypes.POINTER(ctypes.c_void_p),
                          ctypes.POINTER(ctypes.c_void_p)]
api.lp_client_destroy.argtypes = [ctypes.c_void_p]
api.lp_string_free.argtypes = [ctypes.c_void_p]


def invoke(client, method: str, timeout_ms: int = 1000):
    result = ctypes.c_void_p()
    error = ctypes.c_void_p()
    status = api.lp_invoke(client, method.encode(), b"[]", timeout_ms,
                           ctypes.byref(result), ctypes.byref(error))
    value = ctypes.string_at(result).decode() if result else None
    api.lp_string_free(result)
    api.lp_string_free(error)
    return status, value


class Session:
    def __init__(self, case: str, *, delay_init=False, custom_allocator=False,
                 concurrency="single", workers=0):
        instance = f"plain_host_test_{os.getpid()}_{case}"
        env = os.environ.copy()
        env["LOGOS_INSTANCE_ID"] = instance
        os.environ["LOGOS_INSTANCE_ID"] = instance
        if delay_init:
            env["LOGOS_TEST_DELAY_INIT"] = "1"
        if custom_allocator:
            env["LOGOS_TEST_CUSTOM_ALLOCATOR"] = "1"
        argv = [str(HOST), "--name", "plain_host_fixture", "--path", str(FIXTURE),
                "--concurrency", concurrency]
        if workers:
            argv += ["--max-workers", str(workers)]
        self.process = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                        stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                        text=True, env=env)
        self.process.stdin.write("secret\n")
        self.process.stdin.flush()
        self.lines = queue.Queue()
        threading.Thread(target=self._read_output, daemon=True).start()
        api.lp_token_save(b"plain_host_fixture", b"secret")
        self.client = api.lp_client_create(b"plain_host_fixture", b"test", None, None)
        assert self.client

    def _read_output(self):
        for line in self.process.stdout:
            self.lines.put(line.rstrip())

    def waitline(self, needle: str, timeout=6):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                line = self.lines.get(timeout=min(0.2, deadline - time.monotonic()))
            except queue.Empty:
                if self.process.poll() is not None:
                    raise AssertionError(
                        f"host exited {self.process.returncode}: {self.process.stderr.read()}")
                continue
            if needle in line:
                return line
        raise AssertionError(f"host did not print {needle}")

    def close(self):
        api.lp_client_destroy(self.client)
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)


def test_initialization():
    session = Session("init", delay_init=True)
    try:
        session.waitline("INIT_ENTERED")
        status, value = invoke(session.client, "ready", 150)
        assert status != 0, (status, value)
        session.waitline("@logos-load-status")
        assert invoke(session.client, "ready") == (0, "true")
    finally:
        session.close()


def test_allocator():
    session = Session("allocator", custom_allocator=True)
    try:
        session.waitline("@logos-load-status")
        assert invoke(session.client, "ready") == (0, "true")
        assert session.process.poll() is None, "host freed a module-owned result incorrectly"
    finally:
        session.close()


def test_concurrency(mode: str, workers: int):
    session = Session(f"{mode}_{workers}", concurrency=mode, workers=workers)
    calls = []
    try:
        session.waitline("@logos-load-status")

        def slow_call():
            calls.append(invoke(session.client, "slow", 4000))

        first = threading.Thread(target=slow_call)
        first.start()
        session.waitline("SLOW_ENTERED")
        quick = invoke(session.client, "ready", 180)
        assert (quick == (0, "true")) == (mode == "multi" and workers == 2), quick

        if mode == "multi" and workers == 2:
            second = threading.Thread(target=slow_call)
            second.start()
            session.waitline("SLOW_ENTERED")
            assert invoke(session.client, "ready", 180)[0] != 0
            second.join(timeout=5)
            assert not second.is_alive()
        first.join(timeout=5)
        assert not first.is_alive()
        assert all(result == (0, '"ok"') for result in calls), calls
    finally:
        session.close()


def test_single_runs_on_one_thread():
    # Thread-affine modules (Nim refc among them) need every call on one thread.
    session = Session("affinity")
    seen = []
    lock = threading.Lock()
    try:
        session.waitline("@logos-load-status")

        def calls():
            for _ in range(5):
                status, value = invoke(session.client, "thread", 3000)
                with lock:
                    seen.append((status, value))

        callers = [threading.Thread(target=calls) for _ in range(6)]
        for caller in callers:
            caller.start()
        for caller in callers:
            caller.join(timeout=15)
        assert all(status == 0 for status, _ in seen) and len(seen) == 30, seen
        assert len({value for _, value in seen}) == 1, sorted({value for _, value in seen})
    finally:
        session.close()


def alive(pid: int) -> bool:
    # Exited but not yet reaped by its new parent counts as gone.
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    try:
        with open(f"/proc/{pid}/stat") as stat:
            return stat.read().rsplit(")", 1)[1].split()[0] != "Z"
    except FileNotFoundError:
        return True


def test_exits_when_its_parent_dies():
    # A daemon killed outright never stops its hosts; they must notice.
    os.environ["LOGOS_INSTANCE_ID"] = f"plain_host_test_{os.getpid()}_orphan"
    argv = [str(HOST), "--name", "plain_host_fixture", "--path", str(FIXTURE)]
    parent = subprocess.Popen(
        [sys.executable, "-c",
         "import subprocess, sys, time\n"
         "host = subprocess.Popen(sys.argv[1:], stdin=subprocess.PIPE,"
         " stdout=subprocess.DEVNULL)\n"
         "host.stdin.write(b'secret\\n'); host.stdin.flush()\n"
         "print(host.pid, flush=True)\n"
         "time.sleep(60)\n", *argv],
        stdout=subprocess.PIPE, text=True)
    host = int(parent.stdout.readline())
    api.lp_token_save(b"plain_host_fixture", b"secret")
    client = api.lp_client_create(b"plain_host_fixture", b"test", None, None)
    try:
        deadline = time.monotonic() + 10
        while invoke(client, "ready", 200) != (0, "true"):
            assert time.monotonic() < deadline, "host never came up"
        parent.kill()
        parent.wait()
        deadline = time.monotonic() + 5
        while alive(host):
            assert time.monotonic() < deadline, "host outlived its parent"
            time.sleep(0.05)
    finally:
        api.lp_client_destroy(client)
        parent.kill()
        if alive(host):
            os.kill(host, signal.SIGKILL)


test_initialization()
test_allocator()
for case in (("single", 0), ("multi", 1), ("multi", 2)):
    test_concurrency(*case)
test_single_runs_on_one_thread()
test_exits_when_its_parent_dies()
print("plain host: initialization, allocator ownership, worker limits, affinity"
      " and parent death passed")
