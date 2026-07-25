# Configuration

`config/`는 실행 가능한 application의 TOML 예시를 보관합니다. TOML parsing과
schema validation은 `apps/`가 담당하며, `src/`의 reusable component는 typed
options만 받습니다.

## Files

| File | Application | Default Endpoint |
| --- | --- | --- |
| [`echo_server.toml`](echo_server.toml) | `iocp_echo_server` | `127.0.0.1:9000` |
| [`http_server.toml`](http_server.toml) | `iocp_http_server` | `127.0.0.1:8080` |
| [`webapp.toml`](webapp.toml) | `iocp_webapp_server` | `127.0.0.1:8080` |

## Precedence

echo server:

```text
CLI > environment > TOML > named default
```

HTTP server:

```text
CLI > TOML > named default
```

모든 값은 server 시작 전에 검증됩니다. port, queue, buffer, worker count처럼
운영 환경에서 바뀌는 값은 source에 직접 넣지 않고 configuration을 통해
주입합니다.

## Boundaries

- 이 디렉터리의 파일은 실행 예시이며 secret 저장소가 아닙니다.
- production credential과 private endpoint는 commit하지 않습니다.
- component별 기본값은 해당 options type에 남깁니다.
- application별 source precedence는 application configuration에 둡니다.
- socket option을 추가할 때는 생성 시점과 적용 대상을 함께 명시합니다.

사용 가능한 CLI option은 각 실행 파일의 `--help`로 확인합니다.
