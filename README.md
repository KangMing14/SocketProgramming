# Hybrid FTP

A Windows C++ FTP-style client/server for the VNU-HCMUS Networks course. The
control channel uses TCP and file data uses a reliable UDP transport.

## Build and test

```powershell
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Run `build/ftp_server.exe`, then `build/ftp_client.exe`. With no arguments, the
client connects to `127.0.0.1:4567`.

To run the client on a second PC, pass the server's LAN IPv4 address and,
optionally, the control port:

```powershell
./build/ftp_client.exe 192.168.1.20 4567
```

The server listens on all local IPv4 interfaces. Allow the executables through
Windows Firewall on the private network before testing across two PCs.

## Data commands

- `PORT [h1,h2,h3,h4,p1,p2]` binds a one-transfer client UDP socket. With no
  argument, the client selects an endpoint and generates the FTP address. With
  an argument, it validates and binds that exact local endpoint before sending
  it to the server.
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

## Project 2 HTTP client

`http_client` is a raw Winsock HTTP/1.1 client for the Wireshark assignment. It
resolves `www.google.com`, opens a TCP connection to port 80, sends a hand-written
`GET /` request, and prints the response until the server closes the connection.

```powershell
cmake --build build --target http_client
./build/http_client.exe
```

Start Wireshark before running the program. To retain both the DNS lookup and
the HTTP conversation, capture without a filter or use `port 53 or tcp port 80`;
then use `dns || tcp.port == 80` as a display filter.
