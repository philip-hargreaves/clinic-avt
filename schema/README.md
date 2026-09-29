# Wire contract

The engine and the shell talk JSON-RPC 2.0 over a named pipe, one message per frame. `fixtures/`
holds one example of every method and notification. Both sides test against the same files: the
engine in `engine/tests/adapters/ipc/`, the shell's client in
`app/ClinicAVT.Client.Tests/FixtureTest.cs`. A change to a message changes its fixture, and both
test suites then have to agree with it.

## Fixtures

- A request with a reply is `{"request": ..., "response": ...}`, as in `note-tier.json`.
- Some older fixtures are the request alone, as in `session-label.json`.
- A method without parameters has no `params` member, as the client sends it.
- A notification is the message itself, with `method` and `params` and no `id`, as in
  `audio-level.json`.
- The file is named after the method with `/` as `-`. Where a request and a notification share a
  name, the notification keeps the plain name (`reflection-summary.json`) and the request adds
  `-request`; `asr/device` is the one case the other way round (`asr-device.json`,
  `asr-device-notification.json`).
- Values are ones the engine really sends: the same sample session id and times across files, and
  a value the engine computes (such as `tokensPerSecond`) is checked by key only.

## Protocol version

`engine/hello` exchanges `protocolVersion`, and each side accepts only its own number. The number
is `kProtocolVersion` in `engine/src/adapters/ipc/messages.hpp` and `Protocol.ProtocolVersion` in
`app/ClinicAVT.Client/Messages.cs`; the two change in the same commit, with `hello-request.json`
and `hello-response.json`.

Bump it for a change an older peer would get wrong:

- a method, notification or field removed or renamed
- a field whose type, unit or meaning changes
- a parameter that becomes required, or a value the other side must now send
- an error code that changes meaning

Leave it for a change an older peer can ignore: a new method or notification, a new optional
parameter, or a new field in a result or notification. The client reads unknown fields as absent
and ignores unknown notifications.

On a mismatch the engine answers `engine/hello` with an invalid-params error. The shell refuses a
reply with another number, logs the failed attempt and keeps redialling without connecting, so
every request fails with "engine is not connected" until the engine and shell match. They ship
together, so a mismatch means a mixed install.
