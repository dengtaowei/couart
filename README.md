# couart

**one UART, many collaborators**

Linux serial ports are exclusive. That is the wrong default for a desk where
a human, an AI agent, and a flasher all need the same cable.

couart is a small user-space hub: it holds the real port and publishes
**seats** — ordinary PTYs that WindTerm, minicom, and scripts can open as if
they were serial devices.

```
  WindTerm ──► console ─┐
  agent/CI ──► agent   ─┼─► couart ─► /dev/ttyUSB1 ─► board
  logger   ──► watch   ─┘     (exclusive)
```

## Seats

After `couart attach /dev/ttyUSB1 --name demo`:

| Path | Role |
|------|------|
| `$XDG_RUNTIME_DIR/couart/demo/console` | human terminal (rw) |
| `$XDG_RUNTIME_DIR/couart/demo/agent` | scripts / agents (rw) |
| `$XDG_RUNTIME_DIR/couart/demo/watch` | observe only (ro) |

Open `console` in WindTerm as **Serial**. Baud on the PTY is ignored; the
real baud is set when you attach.

## Build

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

## Use

```bash
./build/couart attach /dev/ttyUSB1 --name demo -b 115200
./build/couart list
./build/couart status demo
./build/couart history demo

# rebind device/baud without dropping WindTerm seats
./build/couart port demo /dev/ttyUSB0 --baud 115200

# flasher needs /dev/ttyUSB1 itself
./build/couart suspend demo
esptool.py --port /dev/ttyUSB1 write_flash ...
./build/couart resume demo

./build/couart detach demo
```

You must be in the `dialout` (or equivalent) group to open the physical port.

## Suspend / resume

`suspend` when another program must open `/dev/ttyUSB*` (flasher, ISP).
Seats stay; the board goes quiet. `resume` when that program exits,
including cancel. Console and MCP do not need this. To change device or
baud, use `port`, not suspend.

## MCP (AI agents)

```bash
./build/couart mcp --name demo
```

Stdio JSON-RPC. Cursor example `mcp.json`:

```json
{
  "mcpServers": {
    "couart": {
      "command": "/path/to/couart",
      "args": ["mcp", "--name", "demo"]
    }
  }
}
```

Tools: `serial_port_status`, `serial_send_command`, `serial_write`,
`serial_read`, `serial_output_history`, `serial_wait_for`,
`serial_suspend`, `serial_resume`, `serial_list_ports`,
`serial_bind_port`.

## Why user space

The useful part is **session policy**, not extra kernel ttys: who may write,
when the port is lent to a ROM bootloader, how RX is shared. That belongs in
a process you can restart, not in an out-of-tree module.

## Status

v0.1 — Linux, one hub per named instance, three seats, suspend/resume.
Not a kernel driver. Contributions welcome.

## License

MIT. See [LICENSE](LICENSE).
