# Interoperability Scripts

## HTTP/2 prior knowledge

실행 중인 cleartext HTTP/2 server에 Node.js의 독립 `node:http2` client로
접속해 `GET /health`와 `POST /echo`를 concurrent stream으로 검증한다.

```powershell
node scripts/http2_interop_client.mjs 127.0.0.1 8080
```

성공하면 두 response의 status와 body를 JSON 한 줄로 출력한다. 이 도구는
runtime 구현을 import하지 않으며 Node.js 자체 HTTP/2 stack과의
상호운용만 검증한다.
