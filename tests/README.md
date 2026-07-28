# Tests

별도 test framework 없이 실행 가능한 test binary + CTest를 사용합니다.

## Targets (11개, 모두 Debug/Release 통과)

| Target | Coverage |
|---|---|
| `buffer_tests` | ByteView, BufferSequence, ring/linear receive buffer |
| `execution_tests` | `InlineExecutor`, `ManualExecutor`, `ThreadPoolExecutor`, `SerialExecutor`, `IocpExecutor` |
| `m2_transport_tests` | `TcpListener`, `TcpConnector`, `TcpConnection`, `SendQueue`, echo, shutdown |
| `protocol_tests` | Length-prefixed codec, session, dispatcher |
| `http_tests` | HTTP parser (all split boundaries), router, encoder, session pipeline |
| `http_chunked_tests` | Chunked encoding (all split boundaries), `Expect: 100-continue` |
| `http2_tests` | 모든 session split, stream request 조립, HPACK dynamic table, outbound flow control, close 경합 |
| `webapp_handler_tests` | Login, post CRUD, HTML escape, template render (socket/server 독립) |
| `http_server_tests` | Real loopback HTTP/1.1 + h2c GET/POST concurrent stream |
| `http_configuration_tests` | HTTP CLI/TOML validation |
| `configuration_tests` | Echo CLI/environment/TOML validation |

## Run

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

빌드 아웃풋은 `build/windows-debug/test/`에 있습니다:

```powershell
.\build\windows-debug\test\http_tests.exe
.\build\windows-debug\test\http_chunked_tests.exe
.\build\windows-debug\test\http2_tests.exe
.\build\windows-debug\test\webapp_handler_tests.exe
```

독립 HTTP/2 stack 상호운용:

```powershell
node scripts/http2_interop_client.mjs 127.0.0.1 8080
```

## Test Boundaries

- deterministic state machine과 buffer contract는 unit test
- socket lifecycle은 loopback integration test
- queue saturation, partial completion, cancellation, shutdown을 정상 경로와 같은 비중으로
- 모든 byte split boundary에서 parser 결과 동일성 검증
- handler 단위 테스트는 socket/parser 의존성 없이 독립 실행
- 자체 codec round-trip과 별도로 Node.js `node:http2` 상호운용 검증
