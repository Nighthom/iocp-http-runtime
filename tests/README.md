# Tests

`tests/`는 별도 test framework 없이 실행 가능한 test binary와 CTest 등록을
사용합니다. 작은 unit test와 실제 loopback socket을 사용하는 integration
test를 함께 둡니다.

## Targets

| Target | Main Coverage |
| --- | --- |
| `buffer_tests` | byte view, linear/ring receive buffer, segmented sequence |
| `execution_tests` | executor ordering, saturation, stop modes |
| `m2_transport_tests` | listener, connector, connection, send, shutdown |
| `protocol_tests` | length-prefixed codec, session, dispatcher |
| `http_tests` | parser, router, encoder, HTTP session |
| `http_server_tests` | 실제 loopback HTTP server |
| `configuration_tests` | echo CLI, environment, TOML validation |
| `http_configuration_tests` | HTTP CLI와 TOML validation |

## Run

```powershell
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Release configuration:

```powershell
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
```

특정 test binary를 직접 실행하면 실패 위치를 좁히기 쉽습니다.

```powershell
.\build\windows-debug\http_tests.exe
.\build\windows-debug\m2_transport_tests.exe
```

## Test Boundaries

- deterministic state machine과 buffer contract는 unit test로 검증합니다.
- Windows socket lifecycle은 loopback integration test로 검증합니다.
- queue saturation, partial completion, cancellation, shutdown을 정상 경로와
  같은 비중으로 다룹니다.
- timeout은 무한 대기를 막는 test barrier로 사용하며 성공 조건으로 사용하지
  않습니다.
- 새 public behavior에는 실패 경로와 shutdown 경로를 함께 추가합니다.
