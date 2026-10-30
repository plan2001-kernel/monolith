# Plan2001 MCP test endpoint

The cloud CPU server can expose a small, stateless MCP Streamable HTTP
endpoint at `https://plan2001.com/mcp`. The requested spelling
`https://plan2001.com/mpc` is kept as an alias. Both paths offer the same
`test_connection` and `check_c` tools. The connection tool confirms
connectivity. `check_c` compiles and links submitted Plan 9 C source with
`6c` and `6l`, then returns diagnostics. It does not run the resulting
program. Each build uses the server's networkless, device-masked `websession`
namespace; source is limited to 16 KiB, compiler output to 8 KiB, builds to
10 seconds, and concurrent builds are serialized. Temporary files are
removed after each request from the server's RAM-backed `/tmp` filesystem.

## Install

On an administrator workstation with the Cloud WireGuard peer configured:

```sh
tools/cloud/wg-admin up
tools/cloud/mcp-test
```

The deployment builds and installs only the MCP CGI handler and updates the
`rc-httpd` route. It backs up the prior handler and route under
`/sys/lib/plan2001/old/mcptest/` on the server. It does not rerun the site
setup or restart unrelated services.

## Connect in ChatGPT

Enable Developer Mode if it is available for the account, then open **Settings
→ Apps → Create** and configure a custom app with:

- **Server URL:** `https://plan2001.com/mcp`
- **Authentication:** None

Choose **Scan Tools**, then create the app. In a new chat, select the draft
app and ask it to call `test_connection`. The expected result says the
connection to the Plan2001 MCP test server succeeded. To check C code, ask
the app to call `check_c` with the complete source, for example:

```c
#include <u.h>
#include <libc.h>

void
main(void)
{
	print("Hello from Plan2001\n");
}
```

The `/mpc` alias can be used as the server URL if preferred.

The endpoint implements MCP `initialize`, `notifications/initialized`,
`ping`, `tools/list`, and `tools/call` over JSON POST requests. It is
intentionally stateless and exposes no write operation.
