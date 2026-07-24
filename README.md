# iocp-http-runtime

Windows IOCP 위에 transport, execution, HTTP/1.1, HTTP/2를 직접 조립한 C++17 서버 런타임입니다.

비동기 I/O에서 발생하는 객체 수명, 실행 문맥, 스트림 파싱, backpressure, shutdown 순서를 코드로 확인하기 위한 학습용 구현입니다.

## Quick Start

```powershell
# HTTP/1.1 서버
.\build\windows-debug\bin\iocp_http_server.exe --config config\http_server.toml

# 게시판 웹앱 (인자 없이 실행 가능)
.\build\windows-debug\bin\iocp_webapp_server.exe
```

```powershell
curl.exe http://127.0.0.1:8080/health
# {"status":"ok"}

curl.exe -X POST http://127.0.0.1:8080/echo -H "Content-Type: text/plain" -d "hello"
# hello
```

## Architecture

```
TCP bytes → TcpConnection → HttpSession / H2Session
                ↓                    ↓
         IoContext workers    HttpRouter::Dispatch
                                     ↓
                            application thread pool
                                     ↓
                              route handler → response encoder → send queue
```

IOCP worker는 completion과 transport 상태 전이만 담당합니다. handler는 별도 application thread pool에서 실행하고, connection마다 `SerialExecutor`로 응답 순서를 보존합니다.

## Implemented

**Transport:**
- `AcceptEx`, `ConnectEx`, `WSARecv`, `WSASend` 기반 비동기 socket
- `OVERLAPPED`와 operation 객체의 명시적 수명 관리
- partial send, gathered send (`WSABUF` 배열)
- bounded send queue (overflow → fail-closed)
- ring receive buffer, 연결 drain, coordinated shutdown

**Execution:**
- bounded `ThreadPoolExecutor`, `IocpExecutor`, `SerialExecutor`
- `StopMode::Drain` / `StopMode::CancelPending`
- native completion과 분리된 custom IOCP task packet

**HTTP/1.1:**
- incremental parser (모든 byte split boundary 통과)
- `GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `HEAD`, `OPTIONS`
- `Content-Length`, `Transfer-Encoding: chunked`
- `Expect: 100-continue`
- keep-alive, pipelining, request/header/body 크기 제한
- `400`, `404`, `405`, `413`, `414`, `431`, `500` 오류 응답
- `request_id` / `connection_id` 추적

**HTTP/2:**
- binary frame layer (9-byte header + payload)
- stream state machine (Idle→Open→HalfClosed→Closed)
- HPACK (static table 61개 + dynamic table, literal encoding)
- `SETTINGS`, `HEADERS`, `DATA`, `RST_STREAM`, `WINDOW_UPDATE`, `PING`, `GOAWAY`
- h2c prior knowledge detection
- connection/stream flow control
- HTTP/1.1과 동일한 `HttpRouter` 공유

**Application:**
- TOML 기반 configuration (CLI > TOML > defaults)
- 공통 `config_utils` (CliParser, TOML helpers, config auto-detect)
- `SimpleTemplate` (`{{key}}` 치환 템플릿 엔진)
- 게시판 + 로그인 웹앱 (session cookie, in-memory storage)
- handler 단위 테스트 (socket/server 독립)

## Programs

| Target | Description |
|---|---|
| `iocp_echo_server` | length-prefixed echo server |
| `iocp_echo_client` | blocking CLI echo client |
| `iocp_http_server` | HTTP/1.1 + HTTP/2 server |
| `iocp_webapp_server` | 게시판 + 로그인 웹앱 |

## Build

Windows 10/11, C++17, CMake 3.25+, Ninja. MinGW-w64 GCC 15.2 검증 완료.

```powershell
# Debug
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug

# Release
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release

# Install (배포)
cmake --install build/windows-release --prefix dist
```

빌드 아웃풋은 `bin/`(실행파일), `lib/`(라이브러리), `test/`(테스트)로 분리됩니다.

## Tests

11개 test suite, Debug/Release 모두 통과.

| Test | Coverage |
|---|---|
| `buffer_tests` | ByteView, BufferSequence, ring/linear buffer |
| `execution_tests` | Executor ordering, saturation, stop modes |
| `m2_transport_tests` | TCP listener, connector, connection, echo, shutdown |
| `protocol_tests` | Length-prefixed codec, session, dispatcher |
| `http_tests` | HTTP parser, router, encoder, session |
| `http_chunked_tests` | Chunked encoding at all split boundaries, 100-continue |
| `http2_tests` | Frame header encode/decode, HPACK round-trip |
| `webapp_handler_tests` | Login, post CRUD, HTML escape, template render |
| `http_server_tests` | Loopback HTTP/1.1 server |
| `http_configuration_tests` | HTTP CLI/TOML validation |
| `configuration_tests` | Echo CLI/environment/TOML validation |

## Repository Layout

| Path | Role |
|---|---|
| [`apps/`](apps/README.md) | 실행 파일, composition root, configuration |
| [`config/`](config/README.md) | TOML 설정 예시 |
| [`src/`](src/README.md) | 재사용 가능한 runtime library |
| [`tests/`](tests/README.md) | 단위/통합 테스트 |

## Project Status

`feat/m4-m5-m6-http-reliability` 브랜치 기준. M4(Reliability), M5(HTTP/1.1 Hardening), M6(HTTP/2 Core) 기본 구현 완료. `v0.1.0` 이후 다음 단계로 M7(AI Service), M8(CI/CD + Metrics) 예정.
