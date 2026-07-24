# Repository Guidance

이 저장소는 Windows IOCP 위에 transport, execution, buffering,
HTTP/1.1을 조립한 C++17 runtime입니다. 변경할 때 기능 추가보다 ownership,
ordering, backpressure, shutdown contract 보존을 먼저 확인합니다.

## Read Before Editing

- 공개 범위와 실행법: [`README.md`](README.md)
- application 조립: [`apps/README.md`](apps/README.md)
- source 책임 경계: [`src/README.md`](src/README.md)
- configuration 계약: [`config/README.md`](config/README.md)
- 검증 범위: [`tests/README.md`](tests/README.md)

## Architecture Rules

- reusable mechanism은 `src/`, service composition과 policy는 `apps/`에
  둡니다.
- TOML과 CLI parsing은 application 책임이며 reusable component에는 typed
  options를 주입합니다.
- IOCP worker에서는 completion과 transport 상태 전이만 처리하고 오래
  걸리는 application work를 직접 실행하지 않습니다.
- connection별 ordering이 필요하면 기존 `SerialExecutor` 경계를
  사용합니다.
- pending I/O를 추가하기 전에 operation owner, buffer lifetime,
  cancellation, completion 이후 파괴 시점을 정의합니다.
- connection당 pending receive와 send ordering contract를 깨지 않습니다.
- queue와 buffer에는 명시적인 bound와 saturation behavior를 유지합니다.
- shutdown은 listener stop, connection drain, executor stop, IOCP join의
  순서를 보존합니다.
- reusable core에 singleton 또는 숨은 global state를 추가하지 않습니다.
- 기존 abstraction으로 표현할 수 있는 기능에 새 interface를 만들지
  않습니다.

## Code Style

- C++17과 기존 namespace, ownership type, options pattern을 따릅니다.
- public type과 non-obvious lifecycle contract에는 짧은 문서 주석을
  남깁니다.
- 설명 주석과 사용자용 log message는 기존 한국어 스타일을 유지합니다.
- identifier, protocol token, event code는 영어를 사용합니다.
- unrelated refactor, formatting churn, generated build output을 commit하지
  않습니다.

## Git Workflow

- `main`은 보호 branch이며 직접 commit 또는 push하지 않습니다.
- 최신 `main`에서 `feat/`, `fix/`, `docs/`, `test/`, `refactor/`,
  `chore/` branch를 만듭니다.
- 작업 branch를 push하고 pull request로 merge합니다.
- approval은 필요하지 않지만 unresolved conversation은 모두 해결합니다.
- squash 또는 rebase merge를 사용하고 merge 후 작업 branch를 삭제합니다.
- force push와 `main` 삭제는 금지되어 있습니다.
- 공개된 `v0.1.0` tag를 이동하거나 다시 만들지 않습니다.

## Verification

문서만 변경:

- `git diff --check`
- 수정한 Markdown의 local link 확인

code 또는 CMake 변경:

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

shared runtime, lifecycle, release behavior 변경:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

- configuration 변경에는 precedence와 validation test를 추가합니다.
- transport 변경에는 error, partial completion, cancellation, shutdown
  경로를 함께 검증합니다.
- 완료 시 실행한 command, test 결과, 남은 risk를 보고합니다.

검증을 실행할 수 없으면 생략 사실과 이유를 명확히 남깁니다.
