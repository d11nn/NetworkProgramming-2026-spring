#!/usr/bin/env python3
import asyncio
import contextlib
import http.client
import os
import pathlib
import random
import re
import shutil
import socket
import subprocess
import tempfile
import time
import urllib.parse
import shutil


ROOT = pathlib.Path(__file__).resolve().parents[1]
SAMPLE_ROOT = pathlib.Path("/home/ubuntu/project-3-demo-sample-d11nn")


def pick_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def prepare_local_workspace():
    tempdir = pathlib.Path(tempfile.mkdtemp(prefix="np3-local-suite-"))
    workspace = tempdir / "workspace"
    workspace.mkdir()

    for binary in ["http_server", "console.cgi", "cgi_server.exe"]:
        shutil.copy(ROOT / binary, workspace / binary)

    for cgi_name in ["printenv.cgi", "panel.cgi", "hello.cgi", "welcome.cgi"]:
        shutil.copy(SAMPLE_ROOT / "working_dir" / cgi_name, workspace / cgi_name)

    shutil.copytree(SAMPLE_ROOT / "working_dir" / "test_case", workspace / "test_case")
    return tempdir, workspace


class ManagedProcess:
    def __init__(self, argv, cwd, env=None):
        self.argv = argv
        self.cwd = cwd
        self.env = env
        self.proc = None

    def __enter__(self):
        self.proc = subprocess.Popen(
            self.argv,
            cwd=self.cwd,
            env=self.env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=2)

    def wait_until_ready(self, port, timeout=5):
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                out, err = self.proc.communicate(timeout=1)
                raise RuntimeError(
                    f"process {' '.join(self.argv)} exited early: stdout={out!r} stderr={err!r}"
                )
            with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
                sock.settimeout(0.2)
                try:
                    sock.connect(("127.0.0.1", port))
                    return
                except OSError:
                    time.sleep(0.05)
        raise RuntimeError(f"process {' '.join(self.argv)} did not listen on {port}")


def http_get(port, path, host_header="127.0.0.1"):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    conn.request("GET", path, headers={"Host": host_header})
    response = conn.getresponse()
    body = response.read().decode("utf-8", errors="replace")
    headers = dict(response.getheaders())
    conn.close()
    return response.status, headers, body


def assert_contains(text, needle, message):
    if needle not in text:
        excerpt = text[:1500].replace("\n", "\\n")
        raise AssertionError(f"{message}: missing {needle!r}; excerpt={excerpt}")


def extract_session_text(body, session_id):
    chunks = re.findall(
        rf"document.getElementById\('s{session_id}'\)\.innerHTML \+= '(.*?)';",
        body,
        flags=re.S,
    )
    merged = "".join(chunks)
    return (
        merged.replace("\\n", "\n")
        .replace("\\'", "'")
        .replace("\\\\", "\\")
    )


class FakeShellServer:
    def __init__(self, commands, session_name, fragment_seed=0):
        self.commands = commands
        self.session_name = session_name
        self.port = pick_port()
        self.received = []
        self.server = None
        self.done = asyncio.Event()
        self.random = random.Random(fragment_seed)

    async def __aenter__(self):
        self.server = await asyncio.start_server(self.handle_client, "127.0.0.1", self.port)
        return self

    async def __aexit__(self, exc_type, exc, tb):
        self.server.close()
        await self.server.wait_closed()

    async def send_fragments(self, writer, text):
        fragments = []
        start = 0
        while start < len(text):
            remaining = len(text) - start
            chunk_size = self.random.randint(1, min(4, remaining))
            fragments.append(text[start : start + chunk_size])
            start += chunk_size
        for fragment in fragments:
            writer.write(fragment.encode())
            await writer.drain()
            await asyncio.sleep(0.01)

    async def handle_client(self, reader, writer):
        await self.send_fragments(writer, f"*** {self.session_name} connected ***\n%")
        await self.send_fragments(writer, " ")
        for index, command in enumerate(self.commands):
            line = await reader.readline()
            if not line:
                break
            decoded = line.decode()
            self.received.append(decoded)
            expected = command + "\n"
            if decoded != expected:
                raise AssertionError(
                    f"{self.session_name} expected {expected!r}, got {decoded!r}"
                )

            output = (
                f"echo[{index}]<{self.session_name}> & \"quoted\" '{command}'\n"
                if command != "exit"
                else "bye\n"
            )
            await self.send_fragments(writer, output + "%")
            await self.send_fragments(writer, " ")
            if command == "exit":
                break

        writer.close()
        await writer.wait_closed()
        self.done.set()


async def run_fake_shell_console_test(workspace):
    cases_dir = workspace / "test_case"
    commands = {
        "stress_a.txt": ["setenv PATH bin:.", "printenv PATH", "exit"],
        "stress_b.txt": ["noop", "cat test.html", "exit"],
    }
    for name, lines in commands.items():
        (cases_dir / name).write_text("\n".join(lines) + "\n", encoding="utf-8")

    async with FakeShellServer(commands["stress_a.txt"], "alpha") as alpha, FakeShellServer(
        commands["stress_b.txt"], "beta"
    ) as beta:
        query = urllib.parse.urlencode(
            {
                "h0": "127.0.0.1",
                "p0": str(alpha.port),
                "f0": "stress_a.txt",
                "h1": "127.0.0.1",
                "p1": str(beta.port),
                "f1": "stress_b.txt",
            }
        )
        port = pick_port()
        with ManagedProcess([str(ROOT / "cgi_server.exe"), str(port)], cwd=workspace) as proc:
            await asyncio.to_thread(proc.wait_until_ready, port)
            status, _, body = await asyncio.to_thread(
                http_get, port, f"/console.cgi?{query}"
            )
            if status != 200:
                raise AssertionError(f"cgi_server console returned HTTP {status}")

        await asyncio.wait_for(alpha.done.wait(), timeout=5)
        await asyncio.wait_for(beta.done.wait(), timeout=5)

        assert_contains(body, "<th scope=\"col\">127.0.0.1:", "console header should list hosts")
        assert_contains(body, "document.getElementById('s0')", "session 0 scripts should be present")
        assert_contains(body, "document.getElementById('s1')", "session 1 scripts should be present")
        session0 = extract_session_text(body, 0)
        session1 = extract_session_text(body, 1)
        assert_contains(session0, "alpha connected", "session 0 should stream remote output")
        assert_contains(session0, "<b>setenv PATH bin:.", "session 0 should show commands in bold")
        assert_contains(session0, "echo[0]&lt;alpha&gt; &amp; &quot;quoted&quot;", "session 0 should escape HTML output")
        assert_contains(session1, "beta connected", "session 1 should stream remote output")
        assert alpha.received == [line + "\n" for line in commands["stress_a.txt"]]
        assert beta.received == [line + "\n" for line in commands["stress_b.txt"]]


def run_http_server_env_test(workspace):
    port = pick_port()
    with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)
        status, _, body = http_get(
            port,
            "/printenv.cgi?course_name=NP&project=3",
            "nplinux12.cs.nycu.edu.tw:12345",
        )
        if status != 200:
            raise AssertionError(f"http_server printenv returned HTTP {status}")

    expected = {
        "REQUEST_METHOD = GET",
        "REQUEST_URI = /printenv.cgi?course_name=NP&project=3",
        "QUERY_STRING = course_name=NP&project=3",
        "SERVER_PROTOCOL = HTTP/1.1",
        "HTTP_HOST = nplinux12.cs.nycu.edu.tw:12345",
        f"SERVER_PORT = {port}",
    }
    for line in expected:
        assert_contains(body, line, "CGI env output mismatch")


def run_http_server_panel_test(workspace):
    port = pick_port()
    with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)
        status, _, body = http_get(port, "/panel.cgi", "panel.example.test")
        if status != 200:
            raise AssertionError(f"http_server panel returned HTTP {status}")

    for marker in ["Session 1", "t1.txt", "t2.txt", "t3.txt", "nplinux12"]:
        assert_contains(body, marker, "part1 panel.cgi output mismatch")


def run_http_server_sample_cgi_test(workspace):
    has_figlet = shutil.which("figlet") is not None
    has_cowsay = pathlib.Path("/usr/games/cowsay").exists()
    port = pick_port()
    with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)

        status, _, body = http_get(port, "/hello.cgi", "sample.example.test")
        if status != 200:
            raise AssertionError(f"http_server hello.cgi returned HTTP {status}")
        assert_contains(body, "<h1>Hello</h1>", "hello.cgi output mismatch")

        status, _, body = http_get(port, "/welcome.cgi", "sample.example.test")
        if status != 200:
            raise AssertionError(f"http_server welcome.cgi returned HTTP {status}")
        if has_figlet and has_cowsay:
            assert_contains(body, "Welcome", "welcome.cgi output mismatch")
        else:
            assert_contains(body, "command not found", "welcome.cgi should execute even if local demo deps are missing")


def run_http_server_404_test(workspace):
    port = pick_port()
    with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)

        status, _, body = http_get(port, "/does-not-exist.cgi", "panel.example.test")
        if status != 404:
            raise AssertionError(f"http_server missing CGI returned HTTP {status}")
        assert_contains(body, "Not Found", "missing CGI should return 404 body")

        status, _, body = http_get(port, "/../secret.cgi", "panel.example.test")
        if status != 404:
            raise AssertionError(f"http_server traversal CGI returned HTTP {status}")
        assert_contains(body, "Not Found", "path traversal should be rejected")


def run_http_server_env_matrix_test(workspace, rounds=25):
    port = pick_port()
    with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)
        for round_idx in range(rounds):
            host = f"matrix{round_idx}.example.test:{20000 + round_idx}"
            path = f"/printenv.cgi?case={round_idx}&value=a%2Bb%20{round_idx}"
            status, _, body = http_get(port, path, host)
            if status != 200:
                raise AssertionError(f"http_server matrix request {round_idx} returned HTTP {status}")
            assert_contains(body, f"REQUEST_URI = {path}", "REQUEST_URI should be preserved")
            assert_contains(body, f"QUERY_STRING = case={round_idx}&value=a%2Bb%20{round_idx}",
                            "QUERY_STRING should be preserved")
            assert_contains(body, f"HTTP_HOST = {host}", "HTTP_HOST should match the request header")


def run_cgi_server_panel_test(workspace):
    port = pick_port()
    with ManagedProcess([str(ROOT / "cgi_server.exe"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)
        status, _, body = http_get(port, "/panel.cgi", "panel.example.test")
        if status != 200:
            raise AssertionError(f"cgi_server panel returned HTTP {status}")

    for marker in ["Session 5", "t1.txt", "t5.txt", "nplinux12"]:
        assert_contains(body, marker, "part2 panel.cgi output mismatch")


def run_cgi_server_edge_case_tests(workspace):
    port = pick_port()
    with ManagedProcess([str(ROOT / "cgi_server.exe"), str(port)], cwd=workspace) as proc:
        proc.wait_until_ready(port)

        status, _, body = http_get(port, "/does-not-exist.cgi", "panel.example.test")
        if status != 404:
            raise AssertionError(f"cgi_server missing path returned HTTP {status}")
        assert_contains(body, "Not Found", "cgi_server should reject unknown paths")

        status, _, body = http_get(port, "/console.cgi", "panel.example.test")
        if status != 200:
            raise AssertionError(f"cgi_server empty console returned HTTP {status}")
        assert_contains(body, "NP Project 3 Console", "empty console should still render a page")
        if "document.getElementById('s0')" in body:
            raise AssertionError("empty console should not create session scripts")

        query = urllib.parse.urlencode(
            {"h0": "127.0.0.1", "p0": "12345", "f0": "missing-case.txt"}
        )
        status, _, body = http_get(port, f"/console.cgi?{query}", "panel.example.test")
        if status != 200:
            raise AssertionError(f"cgi_server missing test case returned HTTP {status}")
        assert_contains(
            body,
            "cannot open test_case/missing-case.txt",
            "missing test case should surface a clear error",
        )


async def run_http_server_console_cgi_test(workspace):
    cases_dir = workspace / "test_case"
    case_name = "http_console.txt"
    commands = ["setenv PATH bin:.", "printenv PATH", "exit"]
    (cases_dir / case_name).write_text("\n".join(commands) + "\n", encoding="utf-8")

    async with FakeShellServer(commands, "http-console", fragment_seed=7) as shell:
        query = urllib.parse.urlencode(
            {"h0": "127.0.0.1", "p0": str(shell.port), "f0": case_name}
        )
        port = pick_port()
        with ManagedProcess([str(ROOT / "http_server"), str(port)], cwd=workspace) as proc:
            await asyncio.to_thread(proc.wait_until_ready, port)
            status, _, body = await asyncio.to_thread(
                http_get, port, f"/console.cgi?{query}", "sample.example.test"
            )
            if status != 200:
                raise AssertionError(f"http_server console.cgi returned HTTP {status}")

        await asyncio.wait_for(shell.done.wait(), timeout=5)
        session_text = extract_session_text(body, 0)
        assert_contains(session_text, "http-console", "http_server should execute console.cgi")
        assert_contains(session_text, "<b>setenv PATH bin:.&NewLine;</b>", "http_server console should show commands")
        assert shell.received == [line + "\n" for line in commands]


async def run_console_direct_test(workspace):
    cases_dir = workspace / "test_case"
    case_name = "direct_console.txt"
    commands = ["printenv PATH", "exit"]
    (cases_dir / case_name).write_text("\n".join(commands) + "\n", encoding="utf-8")

    async with FakeShellServer(commands, "direct") as shell:
        env = os.environ.copy()
        env["QUERY_STRING"] = urllib.parse.urlencode(
            {"h0": "127.0.0.1", "p0": str(shell.port), "f0": case_name}
        )
        proc = await asyncio.to_thread(
            subprocess.run,
            [str(ROOT / "console.cgi")],
            cwd=workspace,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=20,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"console.cgi failed: {proc.stderr}")
        await asyncio.wait_for(shell.done.wait(), timeout=5)

    assert_contains(proc.stdout, "Content-type: text/html", "console.cgi should emit CGI header")
    assert_contains(proc.stdout, "document.getElementById('s0')", "console.cgi should emit session scripts")
    session_text = extract_session_text(proc.stdout, 0)
    assert_contains(session_text, "direct", "console.cgi should stream shell output")
    assert_contains(session_text, "<b>printenv PATH", "console.cgi should display sent commands")


async def run_randomized_fake_shell_regression(workspace, rounds=30):
    command_pool = [
        "setenv PATH bin:.",
        "printenv PATH",
        "cat test.html",
        "noop",
        "number test.html",
        "removetag test.html",
    ]
    cases_dir = workspace / "test_case"
    rng = random.Random(314159)

    for round_idx in range(rounds):
        session_count = rng.randint(1, 3)
        query_items = {}
        session_specs = []

        async with contextlib.AsyncExitStack() as stack:
            for session_id in range(session_count):
                commands = [rng.choice(command_pool) for _ in range(rng.randint(2, 4))]
                commands.append("exit")
                case_name = f"fuzz_{round_idx}_{session_id}.txt"
                (cases_dir / case_name).write_text("\n".join(commands) + "\n", encoding="utf-8")

                server = await stack.enter_async_context(
                    FakeShellServer(commands, f"fuzz-{round_idx}-{session_id}",
                                    fragment_seed=round_idx * 10 + session_id)
                )
                query_items[f"h{session_id}"] = "127.0.0.1"
                query_items[f"p{session_id}"] = str(server.port)
                query_items[f"f{session_id}"] = case_name
                session_specs.append((session_id, commands, server))

            query = urllib.parse.urlencode(query_items)
            port = pick_port()
            with ManagedProcess([str(ROOT / "cgi_server.exe"), str(port)], cwd=workspace) as proc:
                await asyncio.to_thread(proc.wait_until_ready, port)
                status, _, body = await asyncio.to_thread(
                    http_get, port, f"/console.cgi?{query}"
                )
                if status != 200:
                    raise AssertionError(
                        f"cgi_server randomized round {round_idx} returned HTTP {status}"
                    )

            for session_id, commands, server in session_specs:
                await asyncio.wait_for(server.done.wait(), timeout=5)
                session_text = extract_session_text(body, session_id)
                assert_contains(
                    session_text,
                    f"fuzz-{round_idx}-{session_id}",
                    "randomized round should stream session banner",
                )
                for command in commands:
                    assert_contains(
                        session_text,
                        f"<b>{command}",
                        "randomized round should display commands in bold",
                    )
                assert server.received == [line + "\n" for line in commands]


def prepare_np_single_workspace():
    tempdir = pathlib.Path(tempfile.mkdtemp(prefix="np3-reliability-"))
    workspace = tempdir / "workspace"
    shutil.copytree(SAMPLE_ROOT / "np_single" / "working_dir_template", workspace)
    cases_dir = workspace / "test_case"
    cases_dir.mkdir(exist_ok=True)
    for case in (SAMPLE_ROOT / "working_dir" / "test_case").iterdir():
        shutil.copy(case, cases_dir / case.name)
    shutil.copy(ROOT / "console.cgi", workspace / "console.cgi")
    shutil.copy(ROOT / "cgi_server.exe", workspace / "cgi_server.exe")
    shutil.copy(ROOT / "http_server", workspace / "http_server")
    return tempdir, workspace


async def run_np_single_test():
    tempdir, workspace = prepare_np_single_workspace()
    ports = [pick_port(), pick_port(), pick_port()]
    servers = []
    try:
        for port in ports:
            servers.append(
                subprocess.Popen(
                    [str(workspace / "np_single_golden"), str(port)],
                    cwd=workspace,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    text=True,
                )
            )

        time.sleep(1)
        for server in servers:
            if server.poll() is not None:
                stdout, stderr = server.communicate(timeout=1)
                raise RuntimeError(
                    f"np_single_golden exited early: stdout={stdout!r} stderr={stderr!r}"
                )

        query = urllib.parse.urlencode(
            {
                "h0": "127.0.0.1",
                "p0": str(ports[0]),
                "f0": "t1.txt",
                "h1": "127.0.0.1",
                "p1": str(ports[1]),
                "f1": "t2.txt",
                "h2": "127.0.0.1",
                "p2": str(ports[2]),
                "f2": "t3.txt",
            }
        )

        port = pick_port()
        with ManagedProcess([str(ROOT / "cgi_server.exe"), str(port)], cwd=workspace) as proc:
            await asyncio.to_thread(proc.wait_until_ready, port, 10)
            status, _, body = await asyncio.to_thread(
                http_get, port, f"/console.cgi?{query}"
            )
            if status != 200:
                raise AssertionError(f"cgi_server np_single returned HTTP {status}")

        markers = [
            "setenv PATH bin:.:/usr/bin",
            "removetag test.html",
            "delayedremovetag test.html",
            "Welcome to the information server.",
        ]
        for marker in markers:
            assert_contains(body, marker, "np_single integration output mismatch")
    finally:
        for server in servers:
            if server.poll() is None:
                server.terminate()
                try:
                    server.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    server.kill()
                    server.wait(timeout=2)
        shutil.rmtree(tempdir)


async def main():
    tempdir, workspace = prepare_local_workspace()
    try:
        run_http_server_env_test(workspace)
        run_http_server_panel_test(workspace)
        run_http_server_sample_cgi_test(workspace)
        run_http_server_404_test(workspace)
        run_http_server_env_matrix_test(workspace)
        run_cgi_server_panel_test(workspace)
        run_cgi_server_edge_case_tests(workspace)
        await run_http_server_console_cgi_test(workspace)
        await run_console_direct_test(workspace)
        await run_fake_shell_console_test(workspace)
        await run_randomized_fake_shell_regression(workspace)
        await run_np_single_test()
        print("reliability_suite: PASS")
    finally:
        shutil.rmtree(tempdir)


if __name__ == "__main__":
    asyncio.run(main())
