# NYCU 2026 Network Programming Projects

This repository contains four C++ networking and system programming projects from the NYCU Network Programming course, Spring 2026.

## Overview

The projects build on one another. They begin with a Unix-like shell, extend it into multi-user TCP servers, expose remote shell sessions through HTTP and CGI, and finally route those sessions through a SOCKS4/4A proxy. Together they cover process control, file-descriptor management, concurrent server design, inter-process communication, asynchronous networking, and application-layer protocols.

All source code needed for review is included directly under [`projects/`](projects/). The original GitHub Classroom repositories remain separate and are not required to browse this repository.

## Repository Structure

| Directory | Project | Main Topics |
| --- | --- | --- |
| [`projects/01-npshell/`](projects/01-npshell/) | NPShell and concurrent TCP shell server | Process execution, pipes, redirection, TCP |
| [`projects/02-rwg-server/`](projects/02-rwg-server/) | Remote Working Ground server | Multi-user server, `select`, shared memory, FIFO |
| [`projects/03-http-cgi-console/`](projects/03-http-cgi-console/) | HTTP/CGI remote console | Boost.Asio, HTTP, CGI, asynchronous I/O |
| [`projects/04-socks-proxy/`](projects/04-socks-proxy/) | SOCKS4/4A proxy | CONNECT, BIND, firewall rules, TCP relay |

## Project Details

### [Project 1 — NPShell and Concurrent TCP Server](projects/01-npshell/)

#### Overview

Project 1 implements a Unix-like command interpreter and then exposes the same shell through a concurrent TCP server. The main challenge is routing data correctly among commands, files, delayed pipes, and network sockets while managing multiple child processes.

#### What I Implemented

- Executes external commands through `fork`, `execvp`, and `waitpid`.
- Supports ordinary pipes (`|`), stdout numbered pipes (`|N`), stdout-and-stderr numbered pipes (`!N`), and file redirection (`>`).
- Merges multiple producers targeting the same future numbered pipe.
- Provides `setenv`, `printenv`, and `exit` built-ins.
- Reaps completed child processes and separates per-connection shell state.
- Uses a process-per-client TCP server; `dup2` maps the accepted socket to stdin, stdout, and stderr.

#### Keywords and Technologies

| Category | Topics |
| --- | --- |
| Keywords | Unix shell, command parsing, process lifecycle, file descriptors, ordinary pipe, numbered pipe, redirection, concurrent server |
| Technologies | C++, POSIX APIs, `fork`, `execvp`, `pipe`, `dup2`, `waitpid`, signals, IPv4 TCP sockets |

#### What I Learned

- How a shell translates command syntax into processes and file-descriptor connections.
- Why every process must close unused pipe ends to avoid deadlocks and delayed EOF.
- How process waiting strategy affects large pipeline output and zombie-process cleanup.
- How the same shell engine can serve both a local terminal and a network client by separating execution logic from I/O endpoints.

### [Project 2 — Multi-User Remote Working Ground Server](projects/02-rwg-server/)

#### Overview

Project 2 extends NPShell into a multi-user remote working environment for up to 30 concurrent users. In addition to running shell commands, connected users can inspect the user list, rename themselves, exchange messages, broadcast messages, and pipe command output directly to another user.

#### What I Implemented

- Preserves all Project 1 shell, pipe, redirection, and environment features.
- Adds `who`, `name`, `tell`, and `yell` built-ins.
- Broadcasts login, logout, rename, and user-pipe events.
- Transfers command output between users with `>N` and consumes it with `<N`.
- Rejects nonexistent users, duplicate names, missing pipes, and duplicate pipes with explicit messages.

Two server architectures implement the same behavior:

1. `np_single_proc.cpp` multiplexes the listening socket and every client socket with `select`; users and anonymous user pipes stay in one process.
2. `np_multi_proc.cpp` forks one process per client; System V shared memory stores the user registry and message queues, `SIGUSR1` triggers message delivery, and named FIFOs carry user-pipe data.

#### Keywords and Technologies

| Category | Topics |
| --- | --- |
| Keywords | Multi-user server, I/O multiplexing, process-per-client, IPC, shared state, synchronization, user pipe, message broadcast |
| Technologies | C++, POSIX sockets, `select`, `fork`, System V shared memory, named FIFO, Unix signals, atomic spin lock |

#### What I Learned

- The trade-offs between a single-process event loop and a multi-process server.
- How shared memory, synchronization, and signals coordinate state across processes.
- How named FIFOs transfer data between users whose sessions run in different processes.
- How to isolate each user's environment variables, delayed pipes, input buffer, and cleanup lifecycle.

### [Project 3 — Asynchronous HTTP/CGI Remote Console](projects/03-http-cgi-console/)

#### Overview

Project 3 builds a web-based remote batch console with Boost.Asio. A browser submits up to five remote shell targets and command files; the server connects to every target concurrently, waits for each shell prompt, sends commands one at a time, and streams the results back to the browser in real time.

#### What I Implemented

- `http_server` asynchronously accepts and parses HTTP requests, prepares CGI environment variables, maps the client socket to CGI output, and executes the allowed CGI program in a child process.
- `console.cgi` parses session parameters from `QUERY_STRING`, connects to remote shell servers asynchronously, waits for each `% ` prompt, and sends the next command from the selected test file.
- Browser output is streamed as escaped JavaScript updates so commands and server responses remain ordered and visibly distinct.
- `cgi_server.exe` integrates the HTTP server, panel page, and remote console into one event-driven process for the course's Windows target.
- Shared parsing, rendering, and remote-session logic is centralized in `console_components.cpp/.hpp`.

The reliability suite exercises HTTP status handling, CGI environment propagation, fragmented TCP reads, multiple concurrent sessions, randomized command batches, and integration with the course-provided remote shell server.

#### Keywords and Technologies

| Category | Topics |
| --- | --- |
| Keywords | Event-driven server, asynchronous I/O, HTTP request parsing, CGI, remote batch execution, streaming response, output ordering, TCP fragmentation |
| Technologies | C++14, Boost.Asio, TCP, HTTP, CGI environment variables, `fork`/`exec`, HTML, JavaScript, Python integration tests |

#### What I Learned

- How asynchronous resolve, connect, read, and write operations form a non-blocking session state machine.
- Why TCP is a byte stream and prompts or messages may be split across multiple reads.
- How CGI connects an HTTP server, process environment, standard output, and generated web content.
- How to preserve output order while several remote sessions update one browser response concurrently.
- Why remote output must be escaped for both HTML and JavaScript contexts.

### [Project 4 — SOCKS4/4A Proxy and CGI Client](projects/04-socks-proxy/)

#### Overview

Project 4 implements a SOCKS4/4A proxy and extends the Project 3 web console into a SOCKS client. The proxy parses binary requests, applies firewall policy, establishes CONNECT or BIND sessions, and relays application traffic without depending on the application protocol carried inside the TCP stream.

#### What I Implemented

- Parses SOCKS4 requests and SOCKS4A domain names.
- Supports CONNECT and BIND, including the two replies required by BIND before relaying traffic.
- Resolves SOCKS4A destinations through Boost.Asio and rejects unsupported versions or commands.
- Applies command-specific IPv4 wildcard rules from `socks.conf`; the file is loaded for every request so rules can change without a server restart.
- Forks a child for each SOCKS client and relays traffic in both directions.
- Extends the Project 3 CGI console with `sh` and `sp` query parameters and a SOCKS4A handshake.
- Handles a shell prompt split across separate TCP reads.

The repository includes automated CONNECT, BIND, firewall-reload, and proxied-CGI tests, plus a manual report covering Firefox CONNECT, active-mode FTP BIND, and CGI-through-SOCKS scenarios.

#### Keywords and Technologies

| Category | Topics |
| --- | --- |
| Keywords | Application-layer proxy, SOCKS4, SOCKS4A, CONNECT, BIND, binary protocol, firewall, wildcard matching, full-duplex relay |
| Technologies | C++17, Boost.Asio, TCP, DNS resolution, `fork`, threads, Python `unittest`, active-mode FTP testing |

#### What I Learned

- How to decode and encode a binary network protocol at the byte level.
- The difference between SOCKS CONNECT and the two-stage SOCKS BIND workflow.
- How SOCKS4A delegates domain-name resolution to the proxy server.
- How a proxy performs bidirectional relay while handling EOF and connection shutdown correctly.
- How per-request firewall loading allows rules to change without restarting the server.

## Technologies Used

| Area | Technologies and Concepts |
| --- | --- |
| Languages | C++14/C++17, Python, HTML, JavaScript |
| Network programming | IPv4, TCP sockets, DNS resolution, client-server architecture, HTTP, CGI, SOCKS4/4A |
| Concurrency | `select`, asynchronous event loops, process-per-client, threads, signals |
| Process control | `fork`, `execvp`, `waitpid`, child reaping, environment variables |
| I/O and IPC | File descriptors, `dup2`, anonymous pipes, numbered pipes, named FIFOs, System V shared memory |
| Libraries and tools | Boost.Asio, GNU Make, Python `unittest`, Python `asyncio` integration tests |
| Reliability topics | Partial TCP reads, output ordering, resource cleanup, error paths, protocol-level tests |

## Source Code Guide

The public snapshot contains implementation, build, configuration, and test files. Private Git metadata, course specifications, provided binaries, presentation files, archives, and generated objects are intentionally excluded. See [`projects/README.md`](projects/README.md) for the snapshot policy.

<details>
<summary><strong>projects/01-npshell — NPShell</strong></summary>

| File | Purpose |
| --- | --- |
| [`Makefile`](projects/01-npshell/Makefile) | Builds `npshell`, `np_simple`, helper commands, and copied `ls`/`cat` executables. |
| [`npshell.cpp`](projects/01-npshell/npshell.cpp) | Local shell entry point and `SIGCHLD` registration. |
| [`np_simple.cpp`](projects/01-npshell/np_simple.cpp) | IPv4 TCP listener and process-per-client shell adapter. |
| [`shell_core.h`](projects/01-npshell/shell_core.h) | Parser, built-ins, environment handling, child lifecycle, FD cleanup, ordinary pipes, numbered pipes, and redirection. |
| [`commands/noop.cpp`](projects/01-npshell/commands/noop.cpp) | No-output command used in pipeline tests. |
| [`commands/number.cpp`](projects/01-npshell/commands/number.cpp) | Line-numbering filter for files or stdin. |
| [`commands/removetag.cpp`](projects/01-npshell/commands/removetag.cpp) | HTML-like tag-removal filter. |
| [`commands/removetag0.cpp`](projects/01-npshell/commands/removetag0.cpp) | Tag-removal filter with stderr diagnostics. |
| [`test.html`](projects/01-npshell/test.html) | Shared command and pipeline fixture. |
| [`.gitignore`](projects/01-npshell/.gitignore) | Excludes generated commands, binaries, tests, scripts, text output, and Markdown. |

</details>

<details>
<summary><strong>projects/02-rwg-server — multi-user shell</strong></summary>

| File | Purpose |
| --- | --- |
| [`Makefile`](projects/02-rwg-server/Makefile) | Detects available parts and builds the shell, TCP servers, and helper commands. |
| [`np_single_proc.cpp`](projects/02-rwg-server/np_single_proc.cpp) | Single-process `select` server, in-memory user registry, chat built-ins, and anonymous-pipe user channels. |
| [`np_multi_proc.cpp`](projects/02-rwg-server/np_multi_proc.cpp) | Process-per-client server using shared memory, a spin lock, signal-triggered message queues, and FIFO user pipes. |
| [`shell_core.h`](projects/02-rwg-server/shell_core.h) | Generalized Project 1 execution engine with per-user environment state and injectable input/output/error FDs. |
| [`npshell.cpp`](projects/02-rwg-server/npshell.cpp) | Local shell adapter retained from Project 1. |
| [`np_simple.cpp`](projects/02-rwg-server/np_simple.cpp) | Simple process-per-client shell server retained from Project 1. |
| [`commands/`](projects/02-rwg-server/commands/) | `noop`, `number`, `removetag`, and `removetag0` shell test programs. |
| [`np_project_2_demo.html`](projects/02-rwg-server/np_project_2_demo.html) | Static two-terminal walkthrough of chat commands and cross-user pipes. |
| [`.gitignore`](projects/02-rwg-server/.gitignore) | Excludes generated executables and local test artifacts. |

</details>

<details>
<summary><strong>projects/03-http-cgi-console — HTTP/CGI console</strong></summary>

| File | Purpose |
| --- | --- |
| [`Makefile`](projects/03-http-cgi-console/Makefile) | Builds Linux `http_server`/`console.cgi`, integrated `cgi_server.exe`, and component tests. |
| [`http_server.cpp`](projects/03-http-cgi-console/http_server.cpp) | Async HTTP listener, request collection, CGI environment setup, fork/exec handling, and response errors. |
| [`console.cpp`](projects/03-http-cgi-console/console.cpp) | Standalone CGI entry point for parallel remote shell sessions. |
| [`cgi_server.cpp`](projects/03-http-cgi-console/cgi_server.cpp) | Integrated HTTP panel and console server with serialized browser writes. |
| [`console_components.hpp`](projects/03-http-cgi-console/console_components.hpp) | Shared request/session data types and `RemoteBatchSession` interface. |
| [`console_components.cpp`](projects/03-http-cgi-console/console_components.cpp) | URL/query parsing, HTML/JavaScript escaping, page generation, command loading, and async remote-session implementation. |
| [`tests/test_components.cpp`](projects/03-http-cgi-console/tests/test_components.cpp) | Focused tests for request parsing, URL decoding, session selection, escaping, and page rendering. |
| [`tests/run_reliability_suite.py`](projects/03-http-cgi-console/tests/run_reliability_suite.py) | HTTP, CGI, concurrency, fragmentation, randomized, and golden-server integration suite. The full integration run requires the excluded course fixtures. |
| [`.gitignore`](projects/03-http-cgi-console/.gitignore) | Separates source from build outputs, local notes, editor files, and provided runtime fixtures. |

</details>

<details>
<summary><strong>projects/04-socks-proxy — SOCKS proxy</strong></summary>

| File | Purpose |
| --- | --- |
| [`Makefile`](projects/04-socks-proxy/Makefile) | Builds `socks_server` and `pj4.cgi` with C++17 and Boost.Asio. |
| [`socks_server.cpp`](projects/04-socks-proxy/socks_server.cpp) | SOCKS4/4A parser, DNS resolution, per-request firewall loading, CONNECT/BIND handling, process isolation, session-duration policy, and bidirectional relay. |
| [`console_components.hpp`](projects/04-socks-proxy/console_components.hpp) | Header-only remote console plus SOCKS4A transport, escaping, query parsing, and split-prompt handling. |
| [`pj4.cpp`](projects/04-socks-proxy/pj4.cpp) | CGI entry point that runs up to five remote sessions through the configured SOCKS server. |
| [`console.cpp`](projects/04-socks-proxy/console.cpp) | Direct-connection Project 3 console retained as a comparison/reference implementation. |
| [`test_socks_server.py`](projects/04-socks-proxy/test_socks_server.py) | Integration tests for CONNECT relay, two-stage BIND relay, live firewall reload, and CGI proxy prompt fragmentation. |
| [`socks.conf`](projects/04-socks-proxy/socks.conf) | Default allow rules for CONNECT and BIND. |
| [`panel_socks.cgi`](projects/04-socks-proxy/panel_socks.cgi) | Python CGI form for five remote sessions plus SOCKS host/port parameters. |
| [`note.md`](projects/04-socks-proxy/note.md) | Manual test report for Firefox, active-mode FTP, CGI proxying, and firewall behavior. |

</details>

## Notes

- The projects target Linux/POSIX networking and system programming environments.
- Assignment specifications, presentation files, compiled binaries, archives, golden executables, and standalone CGI reference programs are not included in the public source directories.
- Small helper-command sources and fixtures required to understand the shell behavior are included as course context.
- Feature descriptions are based on the implementation and included tests, not only on the assignment requirements.
