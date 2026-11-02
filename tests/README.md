# Test status

The test sources currently in this directory were written for the previous
length-prefixed JSON protocol from the original dev branch. They are retained
for reference but are not registered in the current DCAP CMake configuration.

Do not use the old CTest instructions or treat these tests as validation of the
DCAP server. The DCAP integration still needs dedicated tests for:

1. DCAP packet header encoding/decoding and malformed/truncated packets.
2. TCP stream parsing when a packet arrives split across reads or multiple
   packets arrive in one read.
3. Authentication, duplicate usernames, session lifecycle, and ACK behavior.
4. UDP registration tokens, heartbeat expiry, and discovery.
5. Broadcast and private-message routing.
6. File offer/decision/cancel authorization and transfer expiry.
7. UDP file chunk ordering, retransmission/window behavior, and SHA-256
   integrity verification.

Build and tests have intentionally not been run yet. Add the DCAP test suite
before treating the integration as verified.
