# Android → PC data protocol

This document specifies exactly what the Android app sends to `remote_pointer.exe` and what the PC sends back.

## Connection

- Transport: TCP
- PC listens on port `47621` by default (or the port supplied on the PC command line).
- Connect to the PC's local IPv4 address, for example `192.168.1.25:47621`.
- The Android device and PC must be reachable on the same Wi-Fi/LAN.
- Command lines contain ASCII characters. Each command ends with one line-feed byte (`LF`, `0A`). A carriage return immediately before line-feed is also accepted. Encode Unicode text inside the Base64 `TEXT` payload.
- Commands are case sensitive. Use uppercase command names and key/button names. Separate arguments with spaces (tabs and extra surrounding spaces are accepted for control commands). Authentication must match `AUTH <pin>` exactly.
- Always read replies: the PC sends one reply for every complete command line it processes. For smooth movement, the app may keep sending commands while a separate reader drains replies in order; do not wait for a round trip after every movement. Wait for authentication success first.
- Keep outgoing commands ordered on one writer. Bound the movement queue so old gyro samples do not build up during slow connections. Never reorder button events or typed text.
- Recommended: enable `TCP_NODELAY` on the Android socket. The PC enables it on accepted connections.

## Auto-Discovery (UDP Port 47622)

To avoid manual IP typing, the PC receiver supports auto-discovery over UDP:
- **PC UDP Listener & Beacon:** The PC listens on UDP port `47622` and broadcasts a beacon every 2 seconds:
  ```text
  LAZY_POINT_BEACON <hostname> <tcp_port>\n
  ```
- **Android Probe:** When the Android app opens, it sends a UDP broadcast to `255.255.255.255:47622`:
  ```text
  LAZY_POINT_DISCOVER
  ```
  The PC immediately responds with:
  ```text
  LAZY_POINT_OFFER <hostname> <tcp_port>\n
  ```
The Android app extracts the PC's IP and name, and connects automatically!

## Pair/authenticate (With Persistent Device Memory)

When a client connects, the PC sends:

```text
PAIRING_REQUIRED\n
```

### 1. First-Time Pairing (With PIN)
The user enters the eight-digit PIN printed in the PC console. Android sends its unique device ID:

```text
AUTH 01234567 <device_id>\n
```

If the PIN matches, the PC generates a permanent authentication token, saves the device in `paired_devices.txt`, and replies:

```text
OK TOKEN 8f2a1b9c3e4d5f60718293a4b5c6d7e8\n
```

Android securely stores this token in `SharedPreferences`.

### 2. Reconnecting (Zero PIN — 100% Automatic)
Every subsequent time the Android app connects (even if the PC was rebooted or receiver restarted), Android sends:

```text
AUTH_TOKEN <device_id> <saved_token>\n
```

The PC verifies the token against `paired_devices.txt` and replies:

```text
OK\n
```

Authentication completes instantly without prompting for a PIN! If a device is revoked or forgotten, the PC replies `ERR auth\n`.


## Control command format

Each command below is one line. The PC responds `OK\n` when Windows accepts the requested input (or the command has no required effect), `ERR command\n` for invalid data, and `ERR input\n` if input injection fails. See the error table below for connection behavior. `OK` does not confirm that a target application handled the input.

> **Note on High-Frequency `MOVE` Commands:** To support 100 Hz – 200 Hz+ sensor polling without TCP reply queue flooding or latency jitter, the receiver silently processes successful `MOVE` commands without sending `OK\n`. Any error during `MOVE` still returns `ERR command\n` or `ERR input\n`. All other commands (`BUTTON`, `KEY`, `TEXT`, `PING`) continue to reply `OK\n`.

### Move the pointer

```text
MOVE <dx> <dy>\n
```

- `dx` and `dy` are signed base-10 integers from `-500` through `500`.
- Positive `dx` moves right; negative `dx` moves left.
- Positive `dy` moves down; negative `dy` moves up.
- Values are integer relative mouse deltas. Optional `+`/`-` signs are accepted. Fractions, out-of-range values, overflow and numeric suffixes are rejected.
- Example: `MOVE -8 3\n` requests movement left by 8 units and down by 3 units. Windows pointer speed and acceleration can change the actual distance in screen pixels. See [Microsoft's mouse input documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/ns-winuser-mouseinput).
- The Android app should convert sensor motion to deltas, apply sensitivity/dead-zone settings, and send frequent small movements. Send `MOVE 0 0\n` only if needed; it has no movement effect.

### Mouse buttons

```text
BUTTON LEFT DOWN\n
BUTTON LEFT UP\n
BUTTON RIGHT DOWN\n
BUTTON RIGHT UP\n
```

Send `DOWN` when the user presses/touches the button, and `UP` when the press ends. A click/tap is one `DOWN` followed by one `UP`. A drag is `DOWN`, movement commands, then `UP`. Repeating a button's current state is a successful no-op; `UP` cannot release an unrelated physical press when this session has not sent `DOWN`.

Before deliberately disconnecting or leaving control mode, send `UP` for held buttons while the connection still works. The PC also attempts to release this session's held buttons on disconnect, timeout, input failure, or normal Ctrl+C shutdown. A lost Wi-Fi link can take up to the 15-second inactivity deadline to be detected. Forced process termination or Windows blocking input can prevent cleanup; release the physical mouse button if needed.

### Special keys

```text
KEY ENTER\n
KEY BACKSPACE\n
KEY TAB\n
KEY ESC\n
KEY SPACE\n
KEY UP\n
KEY DOWN\n
KEY LEFT\n
KEY RIGHT\n
KEY A\n
```

Supported names are `ENTER`, `BACKSPACE`, `TAB`, `ESC`, `SPACE`, arrow names, and one uppercase letter name `A` through `Z`. A `KEY` command presses and releases the virtual key once. Letter output depends on keyboard layout, Caps Lock and physically held modifiers; `KEY A` does not promise an uppercase character. Use `TEXT` for exact characters, punctuation, and Unicode. Modifier shortcuts such as Ctrl+C are not part of this protocol.

### Type text

```text
TEXT <base64-of-UTF8-bytes>\n
```

Encode the exact UTF-8 bytes of the text using standard Base64 (alphabet `A-Z a-z 0-9 + /`, with `=` padding as required). Do not include quotes or spaces around the encoded value. For example, the UTF-8 bytes for `Hi` are `48 69`; Base64 is `SGk=`:

```text
TEXT SGk=\n
```

To type `Hello world`, send:

```text
TEXT SGVsbG8gd29ybGQ=\n
```

Use one `TEXT` line per newly committed text chunk. Sending the entire text field after each edit would type duplicate characters. Android keyboard composition/prediction should finish before sending committed text; send deletions using `KEY BACKSPACE` where appropriate.

The receiver accepts up to 6000 Base64 characters (4500 UTF-8 bytes) per line. Split longer text at Unicode character boundaries before encoding each chunk. The receiver rejects empty text, invalid Base64/padding, invalid UTF-8, and Unicode control characters U+0000 through U+001F and U+007F through U+009F. Use `KEY ENTER`, `KEY TAB`, etc. for controls. A multi-line paste must be split into text chunks and appropriate key commands. Emoji and other valid Unicode text are supported through UTF-16 input events; the target app decides how to display them.

### Keepalive

```text
PING\n
```

The PC replies `OK\n`. Send `PING` every **5 seconds** while connected, including while stationary or holding a button. It is required to keep an otherwise inactive session open: the PC closes a session after **15 seconds without an accepted command**. Other accepted commands also refresh that deadline. Invalid commands and unfinished lines do not refresh it.

## Errors and deadlines

| Reply | Connection behavior | Android action |
|---|---|---|
| `ERR auth` | Closes | Check the PIN, then reconnect. |
| `ERR command` | Stays open | Fix the command/data; it produced no input. |
| `ERR input` | Closes | Show an input error. Some input may already have been applied. Do not automatically resend. |
| `ERR line_too_long` | Closes | Limit every line to 8192 bytes before `LF`, including an optional `CR`. |
| `ERR timeout` | Closes | Reconnect and authenticate again; keep up with the deadlines below. |

- Authentication must finish within 10 seconds of acceptance.
- An authenticated connection needs an accepted command at least every 15 seconds; send a heartbeat every 5 seconds.
- Once the first byte of a new line arrives, finish that line within 5 seconds. Sending occasional bytes does not extend this deadline.
- A stalled reply write is abandoned after approximately 2 seconds. The connection then closes. Read replies continuously.
- Network errors, orderly disconnects, and shutdown may close the connection without a final error line. Treat EOF or a socket exception as a lost session.
- Deadlines are checked with short polling intervals and can have approximately 100 ms of scheduling tolerance.
- If the connection breaks before a reply arrives, the last command's outcome is unknown. Avoid replaying clicks or text automatically. This protocol does not provide deduplication or exactly-once delivery across reconnections.

## Complete example

`C:` means Android → PC; `S:` means PC → Android. The shown `\n` is one line-feed byte, not the two printed characters backslash and n.

```text
S: PAIRING_REQUIRED\n
C: AUTH 01234567\n
S: OK\n
C: MOVE 12 -4\n
S: OK\n
C: BUTTON LEFT DOWN\n
S: OK\n
C: BUTTON LEFT UP\n
S: OK\n
C: TEXT SGk=\n
S: OK\n
C: KEY ENTER\n
S: OK\n
C: PING\n
S: OK\n
```

## Important implementation details

- TCP is a stream: one `recv()` may contain part of a line, one line, or several lines. Buffer received bytes and split commands at `LF`; do not assume one read equals one command.
- Do not send JSON, raw sensor samples, screen coordinates, or binary packets. The receiver accepts only the line commands specified above.
- The PC currently processes one connected phone at a time. Another connection may wait in the TCP listen queue until the current session ends; only use the connection after its greeting and successful authentication. Close the socket cleanly when leaving remote-control mode.
- Commands cannot contain embedded NUL, embedded CR, non-ASCII bytes or other binary data. Only the final CR in a CRLF terminator is removed. `TEXT` carries Unicode through Base64.
- Windows can reject input to an application running at a higher privilege level, and some apps do not support Unicode input events. Physical modifier keys can also affect input. `ERR input` reports injection failure without promising its cause; see [Microsoft's SendInput documentation](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput).
- The pairing PIN is sent over unencrypted TCP. Use the receiver only on a trusted network.
