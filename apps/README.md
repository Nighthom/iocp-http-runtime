# Applications

`apps/`는 reusable runtime을 실행 가능한 프로그램으로 조립하는 계층입니다.
socket, executor, protocol, logger의 구현은 `src/`에 두고 여기서는 생성 순서,
설정 주입, route와 shutdown 정책을 결정합니다.

## Programs

| Target | Source | Role |
| --- | --- | --- |
| `iocp_echo_client` | `echo_client/` | `ConnectEx`와 transport를 확인하는 client |
| `iocp_echo_server` | `echo_server/` | length-prefixed sample protocol server |
| `iocp_http_server` | `http_server/` | HTTP/1.1 server와 기본 route |

## Composition Responsibilities

application은 다음 객체를 생성하고 연결합니다.

- logger와 sink
- `IoContext`와 application executor
- listener 또는 connector
- connection registry
- protocol session, router, handler
- component별 typed options

application handler는 IOCP worker에서 직접 실행하지 않습니다. HTTP server는
application thread pool과 connection별 `SerialExecutor`를 사용해 handler 실행과
응답 순서를 분리합니다.

## Configuration

- echo application은 CLI, environment, TOML, named default를 조합합니다.
- HTTP application은 CLI, TOML, named default를 조합합니다.
- parsing과 validation은 application configuration module이 담당합니다.
- reusable component는 TOML을 알지 않고 typed options만 받습니다.

설정 항목과 예시는 [`config/`](../config/README.md)를 참고합니다.

## Run

```powershell
.\build\windows-debug\iocp_echo_server.exe --config config\echo_server.toml
.\build\windows-debug\iocp_echo_client.exe
.\build\windows-debug\iocp_http_server.exe --config config\http_server.toml
```

새 서비스를 추가할 때는 runtime 내부에 service policy를 넣기보다 새로운
application composition root에서 기존 component를 조합합니다.
