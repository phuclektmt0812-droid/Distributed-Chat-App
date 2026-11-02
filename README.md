# Distributed-Chat-App

## 1. Overview
A distributed client-server collaboration application developed for the Network Application Development course.
The application aims to support real-time messaging, active user discovery, reliable file transfer, and secure communication.

## 2. Planned Features
- User authentication and online user list
- Broadcast messaging
- Private messaging
- File transfer
- Concurrent client connections
- Network error handling and reconnection
- Message encryption
- User interface for messaging and file transfer

## 3. Technology Stack
- Language: C++
- Networking: TCP/UDP sockets
- Concurrency: Multithreading
- User Interface: HTML, CSS, JavaScript
- Version Control: Git and GitHub

## 4. Project Structure
- `client/`: Client application
- `server/`: Server application
- `protocol/`: Shared communication protocol
- `web/`: User interface
- `tests/`: Test cases
- `docs/`: Technical documentation

## 5. Team
- Member 1: Server and user management
- Member 2: Client and user interface
- Member 3: Protocol, file transfer, and security

## 6. Build and Run
Build and run instructions will be added after the initial implementation is completed.

## 7. Web client
Serve the `web/` directory from a local HTTP server and enter the HTTP bridge
address in the sign-in form. The client uses the bridge's current root routes:
`/login`, `/logout`, `/global_message`, `/private_message`, `/user_list`,
`/events`, `/status`, and `/file_upload`. The bridge only permits browser
origins on `localhost` or `127.0.0.1`.

The file picker reports browser-to-bridge upload progress separately from UDP
delivery. The current bridge responds that file upload is not yet integrated;
an upload response or browser progress must not be treated as recipient
confirmation.
