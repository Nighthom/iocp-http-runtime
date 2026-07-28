# Applications

`apps/`는 reusable runtime을 실행 가능한 프로그램으로 조립합니다.

## Programs

| Target | Source | Role |
|---|---|---|
| `iocp_echo_server` | `echo_server/` | length-prefixed echo server (TOML+CLI+env config) |
| `iocp_echo_client` | `echo_client/` | blocking CLI client (transport 확인용) |
| `iocp_http_server` | `http_server/` | HTTP/1.1 + HTTP/2 server (TOML+CLI config) |
| `iocp_webapp_server` | `webapp/` | 게시판 + 로그인 웹앱 (TOML+CLI, template 기반) |

## Composition

각 application은 다음을 조립합니다:
- logger + sink
- `IoContext` + application executor
- listener / connector
- `ConnectionRegistry`
- protocol session (`HttpSession` / `H2Session`)
- h2c prior-knowledge protocol bootstrap
- `HttpRouter` + route handler
- configuration (TOML → CLI → defaults)

handler는 IOCP worker에서 직접 실행하지 않고 application thread pool에서 실행됩니다.

## Configuration

공통 `src/core/config_utils.h`를 통해 TOML + CLI 파싱을 공유합니다.
각 앱은 `ApplyOption(name, value)` + `Validate(options)`만 정의합니다.

| App | Config File | CLI 예시 |
|---|---|---|
| echo_server | `config/echo_server.toml` | `iocp_echo_server --port 9000` |
| http_server | `config/http_server.toml` | `iocp_http_server --port 8081` |
| webapp | `config/webapp.toml` | `iocp_webapp_server --home-dir path/to/templates` |

## Run

```powershell
.\build\windows-debug\bin\iocp_echo_server.exe --config config\echo_server.toml
.\build\windows-debug\bin\iocp_http_server.exe --config config\http_server.toml
.\build\windows-debug\bin\iocp_webapp_server.exe
```
