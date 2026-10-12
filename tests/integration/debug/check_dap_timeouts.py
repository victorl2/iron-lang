#!/usr/bin/env python3
"""Pin the finite DAP protocol deadlines used by every request path."""
import inspect

import dap_client


EXPECTED = 120
checks = {
    "REQUEST_TIMEOUT": dap_client.REQUEST_TIMEOUT,
    "Client.wait": inspect.signature(dap_client.Client.wait).parameters["timeout"].default,
    "Client.response": inspect.signature(dap_client.Client.response).parameters["timeout"].default,
    "Client.call": inspect.signature(dap_client.Client.call).parameters["timeout"].default,
    "Client.event": inspect.signature(dap_client.Client.event).parameters["timeout"].default,
}

bad = {name: value for name, value in checks.items() if value != EXPECTED}
if bad:
    raise SystemExit("DAP request timeout defaults changed: %r" % bad)

print("DAP request timeout defaults: PASS")
