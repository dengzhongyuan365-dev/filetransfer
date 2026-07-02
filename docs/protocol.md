# LAN File Transfer Protocol

This document describes the wire protocol used by the current C++ implementation.

## Frame Header

Every message is sent as one frame:

```text
0..3    magic: "LFTP"
4       frame protocol version: 1
5       message type
6..7    flags, big endian uint16
8..15   body size, big endian uint64
16..    body bytes
```

Frame bodies are limited to 64 MiB.

## Message Types

```text
1   hello
2   file_begin
3   chunk
4   file_end
5   error
6   ack
7   manifest
8   sync_plan
9   delta
10  delta_begin
11  delta_end
```

Text metadata bodies use newline-separated `key=value` fields. Unknown fields should be ignored unless the current decoder explicitly rejects that field's value.

## Hello

The first frame on a connection is `hello`.

Current format:

```text
lan/1 file
lan/1 sync
```

Legacy bodies are still accepted:

```text
file
sync
```

`file` selects single-file transfer. `sync` selects directory synchronization.

## Single File Transfer

Sender flow:

```text
hello(file)
file_begin
wait ack/error
chunk*
file_end
wait ack/error
```

Receiver flow:

```text
read hello
read file_begin
reply ack(offset/complete) or error
read chunk* unless complete=1
read file_end
reply ack("stored") or error
```

### file_begin

Body:

```text
name=<base file name>
size=<bytes>
sha256=<hex digest>
resume=1|0
source=file|clipboard_image
```

`resume=1` lets the receiver continue from an existing `.part` file. `resume=0` makes the receiver ignore any `.part` file and restart from byte 0.
`source` describes the semantic origin of this file. It defaults to `file` for older senders. `clipboard_image` is used when the sender converted raw clipboard image data into a temporary image file; GUI receivers can copy the completed image back to the clipboard and show a notification.

### file_begin ack

Body:

```text
offset=<bytes>
complete=0|1
```

`offset` tells the sender where to start reading from the source file.

`complete=1` means the receiver already has an identical final file. The sender skips chunks and only sends `file_end` to close the transfer cleanly.

Legacy ack body `ready` is treated as `offset=0 complete=0`.

### chunk

Body:

```text
0..7    chunk offset, big endian uint64
8..     payload bytes
```

The receiver requires each chunk offset to equal its current byte count. This keeps the stream ordered and catches malformed senders.

### file_end

`file_end` has an empty body.

After `file_end`, the receiver verifies:

- received byte count equals `file_begin.size`
- sha256 of the `.part` file equals `file_begin.sha256`

Then it renames the `.part` file into the final target and replies with `ack("stored")`.

## Target File Policy

For single-file receive:

- If the final target already exists and size/hash match, the receiver replies `complete=1` and skips transfer.
- If the final target exists and differs, `allow_overwrite=false` rejects the transfer.
- If the final target exists and differs, `allow_overwrite=true` writes through a `.part` file and replaces the target after verification.
- If a `.part` file exists and `resume=1`, the receiver continues from its size when the size is smaller than the source file.
- If a `.part` file is too large, the receiver restarts from byte 0.

## Directory Sync

Directory sync uses the same frame layer and starts with `hello(sync)`.

High-level flow:

```text
sender -> hello(sync)
sender -> manifest
receiver -> ack
receiver -> sync_plan
sender -> ack
sender -> delta stream per non-skip file
receiver -> ack per applied delta
sender -> ack("sync done")
```

The sync plan may classify files as:

- skip: target already matches
- full: receiver needs the full file content as a delta stream
- delta: receiver has a basis file and asks for delta operations

Delta streams use:

```text
delta_begin
delta*
delta_end
```

`delta_begin` carries the target file metadata needed before applying the stream:

```text
0..7        source size, big endian uint64
8..11       sha256 string byte length, big endian uint32
12..        sha256 string bytes
```

Each `delta` frame carries one operation. It is either a literal payload or a copy range from the receiver's basis file.

`delta_end` carries the final operation count:

```text
0..3        op count, big endian uint32
```

The sender reports the operation count at the end so future implementations can generate delta operations while streaming, without precomputing the whole operation list.

The receiver writes through `.part` files and verifies hashes before committing.

## Error Handling

`error` frames carry a text message. The peer should fail the current transfer and surface the message to the caller.

Cancellation is local API behavior. The current implementation cancels active transfers by closing/shutting down the active connection, which wakes blocking reads and lets the app layer emit cancellation events.
