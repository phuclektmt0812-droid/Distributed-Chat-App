const state = {
    currentUser: null,
    authenticated: false,
    connectionStatus: "disconnected",
    onlineUsers: [],
    currentRecipient: null,
    messages: [],
    activeTransfers: [],
    eventPollingActive: false
};

const elements = {};
let eventPollTimer = null;
let statusPollTimer = null;
let eventPollInFlight = false;
let statusPollInFlight = false;
let sessionGeneration = 0;

const EVENT_POLL_INTERVAL = 800;
const STATUS_POLL_INTERVAL = 5000;

function gatewayUrl(path) {
    const configuredUrl = elements.gatewayUrl.value.trim();
    if (!configuredUrl) {
        throw new Error("Enter the gateway address first.");
    }

    return new URL(path, `${configuredUrl.replace(/\/+$/, "")}/`).toString();
}

async function requestJson(path, options = {}) {
    const response = await fetch(gatewayUrl(path), {
        ...options,
        headers: {
            ...(options.body ? { "Content-Type": "application/json" } : {}),
            ...options.headers
        }
    });

    let payload = {};
    const responseText = await response.text();
    if (responseText) {
        try {
            payload = JSON.parse(responseText);
        } catch {
            throw new Error("The gateway returned an invalid response.");
        }
    }

    if (!response.ok) {
        throw new Error(payload.error || `gateway request failed (${response.status}).`);
    }

    return payload;
}

function showNotice(message, type = "info") {
    elements.notice.textContent = message;
    elements.notice.dataset.type = type;
    elements.notice.hidden = !message;
}

function setConnectionStatus(status, detail = "") {
    state.connectionStatus = status;
    elements.connectionStatus.textContent = detail || status;
    elements.connectionStatus.dataset.status = status;
}

function setAuthenticatedView(isAuthenticated) {
    elements.loginSection.hidden = isAuthenticated;
    elements.chatSection.hidden = !isAuthenticated;
    elements.logoutButton.hidden = !isAuthenticated;
    elements.currentUsername.textContent = isAuthenticated ? state.currentUser : "";
}

function setButtonBusy(button, busy, busyText) {
    if (busy) {
        button.dataset.originalText = button.textContent;
        button.textContent = busyText;
        button.disabled = true;
        return;
    }

    button.textContent = button.dataset.originalText || button.textContent;
    button.disabled = false;
    delete button.dataset.originalText;
}

function normalizeUser(user) {
    if (typeof user === "string") {
        return user;
    }
    if (user && typeof user.username === "string") {
        return user.username;
    }
    if (user && typeof user.name === "string") {
        return user.name;
    }
    return "";
}

async function login() {
    sessionGeneration += 1;
    const username = elements.username.value.trim();
    if (!username) {
        showNotice("Enter a username.", "error");
        return;
    }

    showNotice("Connecting...", "info");
    setButtonBusy(elements.loginButton, true, "Connecting...");

    try {
        const result = await requestJson("/login", {
            method: "POST",
            body: JSON.stringify({ username })
        });

        state.currentUser = result.username || username;
        state.authenticated = true;
        state.messages = [];
        state.onlineUsers = [];
        state.activeTransfers = [];
        state.currentRecipient = null;
        setAuthenticatedView(true);
        setConnectionStatus("connected", "Connected");
        showNotice("");
        renderOnlineUsers();
        renderTransfers();
        selectGlobalConversation();

        await refreshOnlineUsers();
        startEventPolling();
    } catch (error) {
        state.authenticated = false;
        state.currentUser = null;
        showNotice(error.message || "Unable to connect to the gateway.", "error");
        setConnectionStatus("disconnected", "Disconnected");
    } finally {
        setButtonBusy(elements.loginButton, false);
    }
}

async function sendMessage() {
    if (!state.authenticated) {
        showNotice("Log in before sending a message.", "error");
        return;
    }

    if (state.currentRecipient !== null) {
        await sendPrivateMessage();
        return;
    }

    const content = elements.messageInput.value.trim();
    if (!content) {
        return;
    }

    setButtonBusy(elements.sendButton, true, "Sending...");
    try {
        await requestJson("/global_message", {
            method: "POST",
            body: JSON.stringify({ content })
        });
        elements.messageInput.value = "";
        showNotice("");
    } catch (error) {
        showNotice(error.message || "Message could not be sent.", "error");
    } finally {
        setButtonBusy(elements.sendButton, false);
        elements.messageInput.focus();
    }
}

async function sendPrivateMessage() {
    if (!state.authenticated) {
        return;
    }
    if (state.currentRecipient === null) {
        showNotice("Select a recipient.", "error");
        return;
    }

    const content = elements.messageInput.value.trim();
    if (!content) {
        return;
    }

    setButtonBusy(elements.sendButton, true, "Sending...");
    try {
        await requestJson("/private_message", {
            method: "POST",
            body: JSON.stringify({
                recipient: state.currentRecipient,
                content
            })
        });
        elements.messageInput.value = "";
        showNotice("");
    } catch (error) {
        showNotice(error.message || "Private message could not be sent.", "error");
    } finally {
        setButtonBusy(elements.sendButton, false);
        elements.messageInput.focus();
    }
}

async function refreshOnlineUsers() {
    try {
        const result = await requestJson("/user_list");
        state.onlineUsers = Array.isArray(result.users)
            ? result.users.map(normalizeUser).filter(Boolean)
            : [];
        renderOnlineUsers();
    } catch (error) {
        showNotice(error.message || "Unable to refresh the user list.", "error");
    }
}

function renderOnlineUsers() {
    elements.userList.replaceChildren();

    if (state.onlineUsers.length === 0) {
        const empty = document.createElement("li");
        empty.className = "empty-list";
        empty.textContent = "No users are online.";
        elements.userList.append(empty);
        return;
    }

    for (const username of state.onlineUsers) {
        const item = document.createElement("li");
        const button = document.createElement("button");
        const name = document.createElement("span");
        const presence = document.createElement("span");

        button.type = "button";
        button.className = "user-item";
        button.dataset.username = username;
        button.setAttribute("aria-pressed", String(state.currentRecipient === username));
        name.className = "user-name";
        name.textContent = username === state.currentUser ? `${username} (you)` : username;
        presence.className = "presence";
        presence.textContent = "Online";
        button.append(name, presence);
        button.addEventListener("click", () => selectRecipient(username));
        item.append(button);
        elements.userList.append(item);
    }

    updateSelectedUser();
}

function updateSelectedUser() {
    for (const button of elements.userList.querySelectorAll(".user-item")) {
        button.setAttribute(
            "aria-pressed",
            String(button.dataset.username === state.currentRecipient)
        );
    }
}

function selectRecipient(username) {
    if (!username || username === state.currentUser) {
        showNotice("Choose another online user for a private conversation.", "info");
        return;
    }

    state.currentRecipient = username;
    elements.conversationTitle.textContent = `Private conversation with ${username}`;
    elements.conversationDescription.textContent = "Private messages";
    elements.globalConversationButton.setAttribute("aria-pressed", "false");
    updateSelectedUser();
    showNotice("");
    renderConversation();
}

function selectGlobalConversation() {
    state.currentRecipient = null;
    elements.conversationTitle.textContent = "Global chat";
    elements.conversationDescription.textContent = "Messages sent to everyone online";
    elements.globalConversationButton.setAttribute("aria-pressed", "true");
    updateSelectedUser();
    renderConversation();
}

function eventMessageData(event) {
    return event && event.data && typeof event.data === "object" ? event.data : event;
}

function renderMessage(message) {
    const data = eventMessageData(message);
    if (!data || typeof data.content !== "string") {
        return;
    }

    const isPrivate = data.private === true || Boolean(data.recipient);
    state.messages.push({
        sender: typeof data.sender === "string" ? data.sender : "Unknown",
        recipient: typeof data.recipient === "string" ? data.recipient : null,
        content: data.content,
        private: isPrivate,
        timestamp: typeof data.timestamp === "string" ? data.timestamp : new Date().toISOString()
    });
    renderConversation();
}

function messageBelongsToConversation(message) {
    if (state.currentRecipient === null) {
        return !message.private;
    }

    return message.private &&
        ((message.sender === state.currentUser && message.recipient === state.currentRecipient) ||
            (message.sender === state.currentRecipient && message.recipient === state.currentUser));
}

function renderConversation() {
    elements.messages.replaceChildren();
    const visibleMessages = state.messages.filter(messageBelongsToConversation);

    if (visibleMessages.length === 0) {
        const empty = document.createElement("p");
        empty.className = "conversation-empty";
        empty.textContent = "No messages in this conversation yet.";
        elements.messages.append(empty);
        return;
    }

    for (const message of visibleMessages) {
        const item = document.createElement("article");
        const heading = document.createElement("div");
        const sender = document.createElement("strong");
        const timestamp = document.createElement("time");
        const content = document.createElement("p");

        item.className = "message";
        heading.className = "message-heading";
        sender.textContent = message.sender;
        const parsedTimestamp = new Date(message.timestamp);
        timestamp.dateTime = Number.isNaN(parsedTimestamp.getTime())
            ? ""
            : parsedTimestamp.toISOString();
        timestamp.textContent = Number.isNaN(parsedTimestamp.getTime())
            ? message.timestamp
            : parsedTimestamp.toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" });
        content.className = "message-content";
        content.textContent = message.content;
        heading.append(sender, timestamp);
        if (message.private) {
            const privateLabel = document.createElement("span");
            privateLabel.className = "private-label";
            privateLabel.textContent = "Private";
            heading.append(privateLabel);
        }
        item.append(heading, content);
        elements.messages.append(item);
    }

    elements.messages.scrollTop = elements.messages.scrollHeight;
}

function handleEvent(event) {
    const type = event && typeof event.type === "string" ? event.type : "";
    const data = eventMessageData(event);

    switch (type) {
        case "CHAT_MESSAGE":
        case "CHAT_DELIVER":
        case "CHAT_ALL":
        case "CHAT_PRIVATE":
            renderMessage(data);
            break;
        case "USER_LIST":
            if (data && Array.isArray(data.users)) {
                state.onlineUsers = data.users.map(normalizeUser).filter(Boolean);
                renderOnlineUsers();
            } else {
                refreshOnlineUsers();
            }
            break;
        case "LOGIN_SUCCESS":
        case "LOGIN_OK":
            state.authenticated = true;
            break;
        case "LOGIN_FAIL":
            showNotice("The chat server rejected the login.", "error");
            break;
        case "DISCONNECTED":
            state.authenticated = false;
            state.currentUser = null;
            stopEventPolling();
            setAuthenticatedView(false);
            setConnectionStatus("disconnected", "Disconnected");
            showNotice("The chat server disconnected.", "error");
            break;
        case "CHAT_ERROR":
        case "ERROR":
            showNotice(
                (data && (data.error || data.message)) || "The chat server reported an error.",
                "error"
            );
            break;
        case "FILE_OFFER": {
            const transferId = data && (data.transfer_id || data.transferId);
            if (!transferId) break;
            const filename = typeof data.filename === "string" ? data.filename : "a file";
            const incoming = findOrCreateTransfer({ transferId: transferId, filename });
            incoming.size = Number(data.size || 0);
            incoming.status = "Awaiting your decision";
            incoming.pendingDecision = true;
            incoming.decisionInFlight = false;
            renderTransfers();
            showNotice(`Incoming file offer: ${filename}. Choose Accept or Reject in the transfer list.`, "info");
            break;
        }
        case "FILE_PROGRESS":
            updateTransferProgress(data || {});
            break;
        case "FILE_COMPLETE":
            markTransferComplete(data || {});
            break;
        case "FILE_RESULT": {
            const transfer = findOrCreateTransfer(data || {});
            transfer.status = data && data.detail ? data.detail : "Transfer failed or cancelled";
            renderTransfers();
            break;
        }
        default:
            break;
    }
}

async function pollEvents() {
    if (!state.eventPollingActive || eventPollInFlight) {
        return;
    }

    eventPollInFlight = true;
    try {
        const result = await requestJson("/events");
        const events = Array.isArray(result.events) ? result.events : [];
        for (const event of events) {
            handleEvent(event);
        }
    } catch (error) {
        if (state.eventPollingActive) {
            showNotice(error.message || "gateway communication failed.", "error");
            if (error.message && /authentication required|unauthorized/i.test(error.message)) {
                stopEventPolling();
                state.authenticated = false;
                state.currentUser = null;
                setAuthenticatedView(false);
            }
        }
    } finally {
        eventPollInFlight = false;
        if (state.eventPollingActive) {
            eventPollTimer = window.setTimeout(pollEvents, EVENT_POLL_INTERVAL);
        }
    }
}

function startEventPolling() {
    if (state.eventPollingActive) {
        return;
    }
    state.eventPollingActive = true;
    void pollEvents();
}

function stopEventPolling() {
    state.eventPollingActive = false;
    if (eventPollTimer !== null) {
        window.clearTimeout(eventPollTimer);
        eventPollTimer = null;
    }
}

async function refreshConnectionStatus() {
    if (statusPollInFlight) {
        return;
    }

    const generation = sessionGeneration;
    statusPollInFlight = true;
    try {
        const result = await requestJson("/status");
        const connected = result.connected === true;
        const authenticated = result.authenticated === true;
        setConnectionStatus(connected ? "connected" : "disconnected",
            connected ? "Connected to gateway" : "gateway disconnected");

        if (generation !== sessionGeneration) {
            return;
        }

        if (authenticated && result.username) {
            const wasAuthenticated = state.authenticated;
            state.currentUser = result.username;
            state.authenticated = true;
            if (!wasAuthenticated) {
                state.messages = [];
                state.currentRecipient = null;
                setAuthenticatedView(true);
                selectGlobalConversation();
                showNotice("");
                void refreshOnlineUsers();
                startEventPolling();
            } else {
                elements.currentUsername.textContent = result.username;
            }
        } else if (state.authenticated) {
            state.authenticated = false;
            state.currentUser = null;
            stopEventPolling();
            setAuthenticatedView(false);
            showNotice(
                result.last_connection_error || "Your chat session is no longer active. Please log in again.",
                "error"
            );
        }
    } catch {
        setConnectionStatus("disconnected", "gateway unavailable");
    } finally {
        statusPollInFlight = false;
        statusPollTimer = window.setTimeout(refreshConnectionStatus, STATUS_POLL_INTERVAL);
    }
}

function updateTransferProgress(event) {
    const transfer = findOrCreateTransfer(event);
    const percentage = Number(event.progress ?? event.percentage);
    transfer.progress = Number.isFinite(percentage)
        ? Math.max(0, Math.min(100, percentage))
        : transfer.progress;
    transfer.status = typeof event.status === "string" ? event.status : "In progress";
    renderTransfers();
}

function markTransferComplete(event) {
    const transfer = findOrCreateTransfer(event);
    transfer.progress = 100;
    transfer.status = "Complete";
    transfer.downloadUrl = typeof event.download_url === "string" ? event.download_url : "";
    renderTransfers();
    showNotice(`Transfer ${transfer.filename || transfer.id} complete.`, "success");
}

function findOrCreateTransfer(event) {
    const id = event.transferId ?? event.transfer_id;
    let transfer = state.activeTransfers.find(item => String(item.id) === String(id));
    if (!transfer && typeof event.filename === "string") {
        transfer = state.activeTransfers.find(item =>
            item.awaitingTransferId && item.filename === event.filename
        );
        if (transfer && id !== undefined && id !== null) {
            transfer.id = id;
            transfer.awaitingTransferId = false;
        }
    }
    if (!transfer) {
        transfer = {
            id: id ?? `transfer-${Date.now()}`,
            filename: typeof event.filename === "string" ? event.filename : "File transfer",
            progress: 0,
            status: "Waiting for transfer",
            pendingDecision: false,
            decisionInFlight: false
        };
        state.activeTransfers.push(transfer);
    }
    return transfer;
}

async function decideIncomingFile(transfer, accepted) {
    if (!transfer || !transfer.pendingDecision || transfer.decisionInFlight) return;

    transfer.decisionInFlight = true;
    transfer.status = accepted ? "Accepting file..." : "Rejecting file...";
    renderTransfers();

    try {
        await requestJson("/file_decision", {
            method: "POST",
            body: JSON.stringify({ transfer_id: transfer.id, accept: accepted })
        });
        transfer.pendingDecision = false;
        transfer.decisionInFlight = false;
        transfer.status = accepted ? "Accepted; preparing to receive" : "Rejected by you";
        renderTransfers();
        showNotice(
            accepted ? `Accepted ${transfer.filename}; receiving over UDP.` : `Rejected ${transfer.filename}.`,
            accepted ? "success" : "info"
        );
    } catch (error) {
        transfer.decisionInFlight = false;
        transfer.status = "Could not send decision; try again";
        renderTransfers();
        showNotice(error.message || "Could not respond to file offer.", "error");
    }
}

function renderTransfers() {
    elements.transferList.replaceChildren();
    for (const transfer of state.activeTransfers) {
        const item = document.createElement("li");
        const label = document.createElement("span");
        const progress = document.createElement("progress");
        const status = document.createElement("span");

        item.className = "transfer-item";
        label.textContent = transfer.filename || "File transfer";
        progress.max = 100;
        progress.value = transfer.progress;
        progress.setAttribute("aria-label", `${label.textContent} progress`);
        status.textContent = `${transfer.status} (${Math.round(transfer.progress)}%)`;
        item.append(label, progress, status);
        if (transfer.pendingDecision) {
            const actions = document.createElement("div");
            const accept = document.createElement("button");
            const reject = document.createElement("button");

            actions.className = "transfer-actions";
            accept.type = "button";
            accept.className = "button button-primary";
            accept.textContent = "Accept";
            accept.disabled = Boolean(transfer.decisionInFlight);
            accept.addEventListener("click", () => void decideIncomingFile(transfer, true));

            reject.type = "button";
            reject.className = "button button-quiet";
            reject.textContent = "Reject";
            reject.disabled = Boolean(transfer.decisionInFlight);
            reject.addEventListener("click", () => void decideIncomingFile(transfer, false));

            actions.append(accept, reject);
            item.append(actions);
        }
        if (transfer.downloadUrl) {
            const download = document.createElement("a");
            download.href = transfer.downloadUrl;
            download.textContent = "Download received file";
            download.rel = "noopener";
            item.append(download);
        }
        elements.transferList.append(item);
    }
}

function uploadFile() {
    if (!state.authenticated) {
        showNotice("Log in before uploading a file.", "error");
        return;
    }

    const file = elements.fileInput.files[0];
    if (!file) {
        showNotice("Choose a file to upload.", "error");
        return;
    }
    if (!state.currentRecipient) {
        showNotice("Select an online recipient before uploading.", "error");
        return;
    }

    const transfer = {
        id: `upload-${Date.now()}`,
        filename: file.name,
        progress: 0,
        status: "Uploading to gateway",
        awaitingTransferId: true
    };
    state.activeTransfers.push(transfer);
    renderTransfers();
    showNotice(`Uploading ${file.name} to the gateway.`, "info");
    setButtonBusy(elements.uploadButton, true, "Uploading...");

    const formData = new FormData();
    formData.append("file", file, file.name);
    formData.append("recipient", state.currentRecipient);

    const request = new XMLHttpRequest();
    try {
        request.open("POST", gatewayUrl("/file_upload"));
    } catch (error) {
        transfer.status = "Upload failed";
        renderTransfers();
        showNotice(error.message || "Enter a valid gateway address.", "error");
        setButtonBusy(elements.uploadButton, false);
        return;
    }
    request.upload.addEventListener("progress", event => {
        if (event.lengthComputable) {
            transfer.progress = Math.round((event.loaded / event.total) * 100);
            renderTransfers();
        }
    });
    request.addEventListener("load", () => {
        const payload = parseXhrResponse(request);
        if (request.status >= 200 && request.status < 300) {
            if (payload.transfer_id) {
                transfer.id = payload.transfer_id;
                transfer.awaitingTransferId = false;
            }
            transfer.progress = 100;
            transfer.status = "Waiting for recipient to accept";
            showNotice(
                "Uploaded to gateway. Waiting for the recipient to accept the transfer.",
                "success"
            );
        } else {
            transfer.status = "Upload failed";
            showNotice(payload.error || `File upload failed (${request.status}).`, "error");
        }
        renderTransfers();
        setButtonBusy(elements.uploadButton, false);
        elements.fileInput.value = "";
        elements.fileName.textContent = "Choose a file";
    });
    request.addEventListener("error", () => {
        transfer.status = "Upload failed";
        renderTransfers();
        showNotice("Could not communicate with the gateway during upload.", "error");
        setButtonBusy(elements.uploadButton, false);
    });

    try {
        request.send(formData);
    } catch (error) {
        transfer.status = "Upload failed";
        renderTransfers();
        showNotice(error.message || "Could not start the file upload.", "error");
        setButtonBusy(elements.uploadButton, false);
    }
}

function parseXhrResponse(request) {
    if (!request.responseText) {
        return {};
    }
    try {
        return JSON.parse(request.responseText);
    } catch {
        return { error: "The gateway returned an invalid response." };
    }
}

async function logout() {
    sessionGeneration += 1;
    stopEventPolling();
    state.authenticated = false;
    state.currentUser = null;
    state.currentRecipient = null;
    state.messages = [];
    state.activeTransfers = [];
    state.onlineUsers = [];
    setAuthenticatedView(false);
    setConnectionStatus("disconnected", "Disconnected");
    renderConversation();
    renderOnlineUsers();
    renderTransfers();

    try {
        await requestJson("/logout", {
            method: "POST",
            body: JSON.stringify({})
        });
        showNotice("You have logged out.", "info");
    } catch (error) {
        showNotice(`Logged out locally, but gateway logout failed: ${error.message}`, "error");
    }
}

function initialize() {
    elements.gatewayUrl = document.getElementById("gateway-url");
    elements.loginSection = document.getElementById("login-section");
    elements.chatSection = document.getElementById("chat-section");
    elements.loginForm = document.getElementById("login-form");
    elements.username = document.getElementById("username");
    elements.loginButton = document.getElementById("login-button");
    elements.logoutButton = document.getElementById("logout-button");
    elements.currentUsername = document.getElementById("current-username");
    elements.connectionStatus = document.getElementById("connection-status");
    elements.notice = document.getElementById("notice");
    elements.userList = document.getElementById("user-list");
    elements.globalConversationButton = document.getElementById("global-conversation");
    elements.conversationTitle = document.getElementById("conversation-title");
    elements.conversationDescription = document.getElementById("conversation-description");
    elements.messages = document.getElementById("messages");
    elements.messageForm = document.getElementById("message-form");
    elements.messageInput = document.getElementById("message-input");
    elements.sendButton = document.getElementById("send-button");
    elements.uploadForm = document.getElementById("upload-form");
    elements.fileInput = document.getElementById("file-input");
    elements.fileName = document.getElementById("file-name");
    elements.uploadButton = document.getElementById("upload-button");
    elements.transferList = document.getElementById("transfer-list");

    const configuredUrl = window.CHAT_API_BASE_URL;
    if (typeof configuredUrl === "string" && configuredUrl.trim()) {
        elements.gatewayUrl.value = configuredUrl.trim();
    } else if (window.location.protocol === "http:" || window.location.protocol === "https:") {
        elements.gatewayUrl.value = window.location.origin;
    }

    elements.loginForm.addEventListener("submit", event => {
        event.preventDefault();
        void login();
    });
    elements.messageForm.addEventListener("submit", event => {
        event.preventDefault();
        void sendMessage();
    });
    elements.messageInput.addEventListener("keydown", event => {
        if (event.key === "Enter" && !event.shiftKey) {
            event.preventDefault();
            void sendMessage();
        }
    });
    elements.globalConversationButton.addEventListener("click", selectGlobalConversation);
    elements.logoutButton.addEventListener("click", () => void logout());
    elements.uploadForm.addEventListener("submit", event => {
        event.preventDefault();
        uploadFile();
    });
    elements.fileInput.addEventListener("change", () => {
        elements.fileName.textContent = elements.fileInput.files[0]?.name || "Choose a file";
    });

    setAuthenticatedView(false);
    setConnectionStatus("disconnected", "Disconnected");
    selectGlobalConversation();
    renderOnlineUsers();
    renderTransfers();
    void refreshConnectionStatus();
}

document.addEventListener("DOMContentLoaded", initialize);