import http2 from "node:http2";

const host = process.argv[2] ?? "127.0.0.1";
const port = Number(process.argv[3] ?? "8080");
const client = http2.connect(`http://${host}:${port}`);

client.on("error", (error) => {
  console.error(error);
  process.exitCode = 1;
});

function request(headers, body = "") {
  return new Promise((resolve, reject) => {
    const hasBody = body.length !== 0;
    const stream = client.request(headers, {
      endStream: !hasBody,
    });
    let status = 0;
    const chunks = [];

    stream.on("response", (responseHeaders) => {
      status = Number(responseHeaders[":status"]);
    });
    stream.on("data", (chunk) => chunks.push(chunk));
    stream.on("end", () => {
      resolve({
        status,
        body: Buffer.concat(chunks).toString("utf8"),
      });
    });
    stream.on("error", reject);
    if (hasBody) {
      stream.end(body);
    }
  });
}

try {
  const [health, echo] = await Promise.all([
    request({
      ":method": "GET",
      ":path": "/health",
    }),
    request(
      {
        ":method": "POST",
        ":path": "/echo",
        "content-type": "text/plain",
        "content-length": "5",
      },
      "hello",
    ),
  ]);

  if (
    health.status !== 200 ||
    health.body !== '{"status":"ok"}\n' ||
    echo.status !== 200 ||
    echo.body !== "hello"
  ) {
    throw new Error(
      `unexpected responses: ${JSON.stringify({ health, echo })}`,
    );
  }

  console.log(JSON.stringify({ health, echo }));
} finally {
  client.close();
}
