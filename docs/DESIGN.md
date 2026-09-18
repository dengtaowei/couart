# Design

couart is a **user-space serial session layer**.

Linux still treats a UART as a single exclusive `open()`. That matched one
human and one `minicom`. It does not match a desk where a person, an agent,
a logger, and a flasher all need the same wire.

## What we publish

For each physical port the hub holds, three **seats** appear as PTYs:

| Seat      | Access | Who                          |
|-----------|--------|------------------------------|
| `console` | rw     | WindTerm / minicom / picocom |
| `agent`   | rw     | scripts and AI tools         |
| `watch`   | ro     | logs, observers              |

The physical node (`/dev/ttyUSB0`, …) stays exclusively opened by the hub.
Clients never fight `TIOCEXCL` on copper.

## Data flow

- UART RX is fanned out to every seat.
- Writable seats fan in to the UART. Seat TX is not mirrored to other seats.
- `watch` never transmits.

There is no kernel module. Policy (who writes, when the port is yielded)
stays in user space so it can change without DKMS or signed modules.

## Yielding the port

`couart suspend <name>` closes the physical fd so `esptool`, `stm32flash`,
or a vendor ISP can take the real node. Seats stay up and print a banner.
`couart resume <name>` reclaims the UART.

A future `flash` seat can make this automatic when that PTY is opened.

## Non-goals (v0.1)

- Windows COM pair emulation
- Kernel tty mux
- Board-specific test scripts

Agents use `couart mcp` (stdio JSON-RPC) against a named hub instance.

## Why not a kernel module

A kmod can mint extra ttys. It cannot host writer policy, suspend/resume
for flashers, history, or agent APIs without dragging that policy into the
kernel. The product metaphor (“several exclusive virtual ports”) is
delivered with PTYs. The kernel keeps doing what it already does: one
real UART.
