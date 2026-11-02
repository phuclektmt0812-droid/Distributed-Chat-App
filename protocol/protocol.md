{
  "type": "LOGIN",
  "username": "alice",
  "password": "example"
}
{
  "type": "CHAT_ALL",
  "content": "Hello everyone!"
}
{
  "type": "CHAT_PRIVATE",
  "recipient": "bob",
  "content": "Can we discuss the assignment?"
}
{
  "type": "CHAT_DELIVER",
  "message_id": "msg-123",
  "sender": "alice",
  "recipient": "bob",
  "content": "Hello!",
  "timestamp": "2026-10-31T09:00:00"
}

## File-transfer negotiation

Clients bind their UDP socket before login and include its bound port in the
`LOGIN` payload as `udp_port`. A client that does not register a UDP port can
still chat, but cannot initiate or accept file transfers.

The sender requests an offer over TCP:

```json
{
  "type": "FILE_START",
  "payload": {
    "transfer_id": 12345,
    "recipient": "bob",
    "filename": "report.pdf",
    "total_size": 2048,
    "udp_port": 53001
  }
}
```

The server forwards the offer to the recipient, including the sender's IP and
registered UDP port. The recipient responds over TCP with `FILE_ACCEPT` or
`FILE_REJECT`, carrying the same `transfer_id`. The server validates the
responding username and relays the result to the sender. An accepted response
includes the recipient's IP address and registered UDP port. The sender waits
up to ten seconds for this response before cancelling the pending transfer.

Once accepted, the sender transmits UDP data packets using stop-and-wait
acknowledgements. UDP packets contain the `DCHT` magic bytes, packet type,
transfer ID, sequence number, payload length, checksum, and payload, with
integer fields in network byte order. The receiver/storage workflow is a
separate implementation step; clients currently expose incoming `FILE_START`
events so the application can accept or reject an offer.