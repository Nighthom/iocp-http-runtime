# Runtime Source Guide

`src/`는 application에 종속되지 않는 IOCP runtime과 protocol component를
보관합니다. 코드를 읽을 때는 socket API 호출보다 각 계층이 소유하는 상태와
실행 위치를 먼저 확인합니다.

## Modules

| Module | Responsibility |
| --- | --- |
| `core/` | structured logging과 sink |
| `platform/windows/` | Winsock lifecycle과 socket RAII |
| `runtime/` | IOCP handle, worker, completion dispatch |
| `execution/` | task submission, thread pool, serialization |
| `buffer/` | borrowed byte view와 bounded receive storage |
| `transport/` | listener, connector, connection, send queue |
| `protocol/sample/` | length-prefixed protocol proof |
| `protocol/http/` | HTTP parser, router, session, response encoder |

## Request Flow

```text
AcceptEx
  -> TcpListener
  -> TcpConnection
  -> WSARecv completion
  -> IProtocolSession::Feed
  -> HttpRequestParser
  -> SerialExecutor
  -> application ThreadPoolExecutor
  -> HttpRouter / handler
  -> HttpResponseEncoder
  -> bounded SendQueue
  -> WSASend completion
```

## Execution Boundaries

- IOCP worker는 completion과 transport 상태 전이를 처리합니다.
- application worker는 handler와 service work를 실행합니다.
- connection별 `SerialExecutor`는 요청 처리와 응답 순서를 직렬화합니다.
- executor saturation은 명시적인 submit status로 호출자에게 전달됩니다.

completion callback에서 오래 걸리는 application work를 직접 실행하지 않습니다.

## Ownership Boundaries

- application server는 `IoContext`, executor, listener, registry를 조립합니다.
- pending operation은 완료될 때까지 connection과 operation buffer 수명을
  보존합니다.
- `TcpConnection`은 receive state와 send queue를 관리하고 receive handler를
  호출합니다.
- application이 만든 receive handler closure는 connection에 연결된 protocol
  session 수명을 보존합니다.
- `SendQueue`는 queued buffer와 partial send offset을 관리합니다.
- `ConnectionRegistry`는 active connection을 추적하고 shutdown drain을
  관찰합니다.
- `ByteView`와 `BufferSequence`는 storage를 소유하지 않습니다.

비동기 호출을 추가할 때는 API 호출보다 먼저 operation owner, cancellation,
completion 이후 파괴 시점을 정합니다.

## Where to Change

| Goal | Start Here |
| --- | --- |
| IOCP worker와 stop packet | `runtime/io_context.*` |
| accept/connect lifecycle | `transport/tcp_listener.*`, `tcp_connector.*` |
| receive/send/close lifecycle | `transport/tcp_connection.*` |
| partial/gathered send | `transport/send_queue.*` |
| task ordering과 backpressure | `execution/` |
| HTTP message boundary | `protocol/http/http_request_parser.*` |
| route와 handler dispatch | `protocol/http/http_router.*` |
| HTTP connection state | `protocol/http/http_session.*` |

새 abstraction은 실제 교체 가능성, 중복 제거, 테스트 경계 중 하나를 분명히
개선할 때 추가합니다.
