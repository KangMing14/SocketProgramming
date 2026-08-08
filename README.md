# Hybrid FTP

A Windows C++ FTP-style client/server for the VNU-HCMUS Networks course. The
control channel uses TCP and file data uses a reliable UDP transport.

## Build and test

```powershell
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run `build/ftp_server.exe`, then `build/ftp_client.exe`. The client connects to
`127.0.0.1:4567`.

## Data commands

- `PORT` binds a one-transfer client UDP socket and sends the generated FTP
  address to the server.
- `PASV` requests a one-transfer server UDP socket.
- `STOR <local-path> [remote-path]` uploads a file. The remote name defaults to
  the local filename.
- `RETR <remote-path> [local-path]` downloads a file. The local name defaults to
  the remote filename. If an explicit local path is an existing directory, the
  remote filename is saved inside it.

A fresh `PORT` or `PASV` command is required before every transfer. Paths with
spaces are not supported by the interactive client.

Before `RETR` starts, the client verifies that the local destination can be
created or replaced. The final RDT packet is acknowledged only after the
temporary download is atomically committed, so a local save failure produces a
failed transfer instead of a misleading `226 Transfer complete`.

For `PORT` uploads, the server sends a checksummed RDT `SYN` from the same
ephemeral UDP socket that receives the file. The client learns that source
endpoint, acknowledges it, and sends `DATA` packets back through its advertised
socket. The final data packet carries `FIN`; empty files use a zero-length
`DATA|FIN` packet.
