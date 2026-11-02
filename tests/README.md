# Test scenarios

Build and run the automated tests on Windows with CMake, a C++17 compiler, and
the `nlohmann_json` CMake package available:

```powershell
cmake -S . -B build
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

The connection, login, and chat executables start an in-process server on an
available local TCP port. Login rejection currently covers empty credentials
and duplicate usernames; the server's current temporary login implementation
does not validate passwords against an account store.

Manual end-to-end file-transfer scenarios:

1. Log in two clients with registered UDP ports, offer a file, and accept it.
2. Verify the received bytes, size, and checksum match the source file.
3. Drop a UDP data packet and verify the sender retransmits it.
4. Repeat with a rejected offer, an unknown transfer ID, and an interrupted
   connection.

The current file-transfer receiver is not implemented, so complete file
delivery, retransmission, and receiver checksum scenarios cannot pass yet. The
automated file-transfer test instead covers UDP packet encoding/decoding,
corruption and truncation checks, repeated packet decoding, and rejection of a
zero transfer ID by the server.
