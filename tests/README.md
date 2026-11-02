# Test status

The test sources currently in this directory were written for the retired length-prefixed JSON protocol. They are retained for reference and are not registered in the DCAP CMake configuration.

A DCAP test suite is still required for:

1. Packet header encoding/decoding and malformed/truncated packets.
2. TCP stream reads split across multiple recv calls and coalesced packets.
3. Authentication, duplicate usernames, session lifecycle, and ACK handling.
4. UDP registration tokens, heartbeat, and timeout behavior.
5. Broadcast/private-message routing.
6. File-offer authorization, accept/reject/cancel, and transfer expiry.
7. UDP chunk ordering, ACK bitmap, retransmission/window behavior, and SHA-256 integrity.
8. HTTP bridge login, chat, user list, event polling, and multipart upload.
9. End-to-end two-client transfer through the server UDP relay.

Do not treat the old JSON test sources as validation of DCAP. Build and tests have intentionally not been run yet.
