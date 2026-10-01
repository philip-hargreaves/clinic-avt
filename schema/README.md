# Wire contract

The engine and the shell exchange JSON-RPC 2.0 messages over a named pipe. `fixtures/` holds
one example of every method and notification, and both sides test against the same files.

| Side | Test |
|---|---|
| Engine | `engine/tests/adapters/ipc/` |
| Shell client | `app/ClinicAVT.Client.Tests/FixtureTest.cs` |

## Fixtures

A file is named after its method, with `/` written as `-`.

| Kind | Shape | Example |
|---|---|---|
| Request with a reply | `{"request": ..., "response": ...}` | `note-tier.json` |
| Request alone | The request | `session-label.json` |
| Notification | The message, with no `id` | `audio-level.json` |

## Protocol version

Both sides send `protocolVersion` in `engine/hello` and accept only their own number. It is set
in `engine/src/adapters/ipc/messages.hpp` and `app/ClinicAVT.Client/Messages.cs`, which change
together.

| Change | Bump the version? |
|---|---|
| A method, notification or field removed or renamed | Yes |
| A field's type, unit or meaning changed | Yes |
| A parameter made required | Yes |
| An error code's meaning changed | Yes |
| A new method, notification, optional parameter or result field | No |
