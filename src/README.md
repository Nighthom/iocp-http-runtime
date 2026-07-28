# Runtime Source Guide

`src/`는 application에 종속되지 않는 재사용 가능한 runtime component입니다.

## Modules

| Module | Responsibility |
|---|---|
| `core/` | structured logging (`Logger`, `ILogSink`, `StreamLogSink`, `FileLogSink`), 공통 config utils |
| `platform/windows/` | Winsock lifecycle (`WinsockRuntime`)와 socket RAII (`SocketHandle`) |
| `runtime/` | IOCP handle, worker thread, completion dispatch (`IoContext`, `CompletionOperation`) |
| `execution/` | task submission과 admission (`IExecutor`, `ThreadPoolExecutor`, `SerialExecutor`, `IocpExecutor`) |
| `buffer/` | borrowed byte view (`ByteView`), `BufferSequence`, bounded receive storage (`ReceiveBuffer`, `RingReceiveBuffer`) |
| `transport/` | `TcpListener`, `TcpConnector`, `TcpConnection`, `ConnectionRegistry`, `SendQueue` |
| `protocol/` | protocol-transport 경계 (`IProtocolSession`)와 bounded preface bootstrap |
| `protocol/http/` | HTTP/1.1 parser, router, session, response encoder, `SimpleTemplate` |
| `protocol/http2/` | frame codec, connection HPACK, stream state, outbound scheduler, `H2Session` |
| `protocol/sample/` | length-prefixed protocol (contract proof) |

## Request Flow

```
HTTP/1.1:
  AcceptEx → TcpListener → TcpConnection → WSARecv → HttpSession
    → HttpRequestParser → SerialExecutor → ThreadPoolExecutor
    → HttpRouter → handler → HttpResponseEncoder → SendQueue → WSASend

HTTP/2:
  AcceptEx → TcpListener → TcpConnection → PrefaceProtocolBootstrap
    → H2Session ring → FrameCodec → HPACK decoder → stream request assembly
    → application pool → HttpRouter → handler
    → H2OutboundScheduler → HPACK encoder → HEADERS/DATA → SendQueue
```

## Execution Boundaries

- IOCP worker: completion과 transport 상태 전이만 처리
- Application worker: handler와 service work 실행
- `SerialExecutor`: HTTP/1.1 connection별 요청/응답 순서 직렬화
- HTTP/2: stream handler 병렬 실행, connection HPACK/flow state는
  session과 outbound scheduler에서 직렬화
- executor saturation: `SubmitStatus`로 호출자에게 전달

## Configuration Pattern

`src/core/config_utils.h/.cpp`:
- `CliParser` — `--key=value`, `--key value`, positional arg 파싱
- `ParseUnsigned` — 부호 없는 정수 + 범위 검증
- `ReadTomlInt/Str`, `OptionalTable`, `RejectUnknownKeys` — TOML table reader
- `FindConfigFile` — `--config` CLI 또는 exe 기준 auto-detect
- `TomlKeyToCli` — TOML underscore key → CLI hyphen key 변환

각 앱은 `ApplyOption(name, value)`와 `Validate(options)`만 정의하면 됩니다.

## Where to Change

| Goal | Start Here |
|---|---|
| IOCP worker / stop | `runtime/io_context.*` |
| accept / connect | `transport/tcp_listener.*`, `tcp_connector.*` |
| recv / send / close | `transport/tcp_connection.*`, `send_queue.*` |
| task ordering | `execution/` |
| HTTP/1.1 parsing | `protocol/http/http_request_parser.*` |
| routing / dispatch | `protocol/http/http_router.*` |
| connection state | `protocol/http/http_session.*` |
| HTTP/2 frames | `protocol/http2/http2_frames.*`, `http2_stream.*` |
| HPACK | `protocol/http2/http2_hpack.*` |
| HTTP/2 response/flow control | `protocol/http2/http2_outbound.*` |
| h2c protocol 선택 | `protocol/preface_protocol_bootstrap.*` |
| template rendering | `protocol/http/simple_template.*` |
| config / CLI | `core/config_utils.*` |
