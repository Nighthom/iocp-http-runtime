# WebApp

간단한 게시판 + 로그인을 제공하는 HTTP/1.1 웹 애플리케이션입니다.
`HttpRouter`와 템플릿 엔진 위에 조립된 self-contained 서버로,
IOCP transport부터 HTML 렌더링까지 전체 스택을 보여줍니다.

## 실행

```powershell
# 인자 없이 (config 자동 감지 + home_dir 컴파일 타임 경로)
.\build\windows-debug\bin\iocp_webapp_server.exe

# config 지정
.\build\windows-debug\bin\iocp_webapp_server.exe --config config\webapp.toml

# CLI override
.\build\windows-debug\bin\iocp_webapp_server.exe --port 3000 --home-dir apps\webapp\templates
```

## 구조

```
apps/webapp/
├── main.cpp              ← CLI argument parsing, config loading
├── configuration.h/.cpp   ← TOML+CLI → WebAppOptions 변환
├── webapp.h/.cpp          ← 서버 state: session/auth/post storage
├── board_handlers.h/.cpp  ← handler 함수: socket/parser 독립, 단위 테스트 가능
└── templates/             ← HTML 템플릿 ({{key}} 치환)
    ├── login.html
    ├── board.html
    ├── post_detail.html
    ├── post_list.html
    └── write_form.html
```

## Routes

| Method | Path | Description |
|---|---|---|
| `GET` | `/` | `/login`으로 redirect |
| `GET` | `/login` | 로그인 폼 |
| `POST` | `/login` | 로그인 처리 (session cookie 발급) |
| `GET` | `/logout` | 로그아웃 (session 제거) |
| `GET` | `/board` | 게시판 목록 |
| `GET` | `/write` | 글쓰기 폼 |
| `POST` | `/write` | 게시글 작성 |
| `GET` | `/post?id=N` | 게시글 상세 |
| `GET` | `/style.css` | 스타일시트 |

## 테스트 계정

| ID | Password |
|---|---|
| `admin` | `admin123` |
| `user` | `pass123` |

## Configuration

`config/webapp.toml`:

```toml
schema_version = 1

[server]
address = "127.0.0.1"
port = 8080
backlog = 128
shutdown_timeout_ms = 10000
```

HTTP 인프라 설정(io_workers, parser limits 등)은 `HttpServerOptions` 기본값을
사용합니다. 웹앱은 port, address, home_dir만 노출합니다.

CLI options: `--port`, `--address`, `--home-dir`, `--shutdown-timeout-ms`, `--config`, `--help`

## 설계

- **state vs handler 분리**: `WebAppServer`는 session/auth/post storage를,
  `board_handlers`는 HTML 렌더링을 담당합니다.
- **handler 단위 테스트**: `board_handlers` 함수는 socket/parser 의존성 없이
  `HttpRequest` 대신 typed parameter를 받아 독립적으로 테스트할 수 있습니다.
- **템플릿**: `{{key}}` 치환 엔진으로 HTML을 C++ 코드에서 분리했습니다.
- **저장소**: 모든 데이터는 in-memory (`std::vector<Post>`, `std::unordered_map`).
  서버 재시작 시 데이터는 소멸됩니다.
