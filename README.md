# NetBoX - User application (C++17)

This directory contains the NetBoX User application. The Directory Server (DS) is
provided by the course. User instances exchange metadata with the DS over UDP and
TCP, and transfer file contents directly between peers over TCP.

## Build and run

```sh
make
./user -m 5000
./user -m 5000 -n 193.136.138.142 -p 59000
```

`-m` is mandatory and selects the local peer TCP port. `-n` and `-p` select the
Directory Server IPv4 address and port. Published files are read from the current
working directory. Downloaded files are written there using their original names;
an existing file is never overwritten.

## Commands

```text
login UID password
publish filename label
remove filename
list
versions filename
where filename
get filename
logout
unregister
exit
```

`where` first reads the resource versions over TCP, then sends a UDP `WHR filename
UID` request for each version reported online. It displays the IP address and peer
TCP port returned by `RWH`. `get` performs the same discovery, tries the available
peers in order, and saves the first successful transfer as `filename`.

## Phase III protocol behavior

- `versions` sends `VRS filename` over TCP and validates the `RVR` status and each
  returned UID, file size, label, and availability.
- `where` and `get` recheck each online publisher with `WHR`; stale or offline
  publishers returned by the versions query are skipped.
- The peer TCP server validates `GET filename` requests and responds with
  `RGT OK Fsize\n` followed by exactly `Fsize` binary bytes. Missing files return
  `RGT NOK\n`; malformed requests return `RGT ERR\n`.
- The peer server runs independently from the command interface and handles up to
  16 concurrent incoming transfers. A `get` command can therefore run while the
  application continues serving other peer requests.
- Downloads validate the advertised size (0 to 10,000,000 bytes), handle partial
  socket reads and writes, remove incomplete files after transfer failures, and
  do not overwrite an existing destination.

## Limits

- UID: exactly 6 decimal digits.
- Password: exactly 8 ASCII letters or digits.
- Filename: up to 24 characters, with a 3-character alphanumeric extension.
- Label: 1 to 20 ASCII letters, digits, `-` or `_`.
- File size: 0 to 10,000,000 bytes.
- `list`: up to 50 unique filenames, as specified by the DS protocol.

Each command performs its DS messaging sequentially. The peer server continues
running on its own thread while the user enters commands or waits for a DS response.
