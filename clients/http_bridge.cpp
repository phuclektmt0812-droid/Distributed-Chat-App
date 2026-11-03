#include "http_bridge.h"
#include "../protocol/packet.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <ws2tcpip.h>
#else
#include <sys/time.h>
#endif

using nlohmann::json;
namespace {
std::uint64_t nowMs() { return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }
constexpr std::size_t MAX_HEADER = 16384;
constexpr std::size_t MAX_BODY = 110ULL * 1024ULL * 1024ULL;
bool sendBytes(SocketHandle s, const char* data, std::size_t size) {
    std::size_t offset = 0;
    while (offset < size) {
        const int amount = static_cast<int>(std::min<std::size_t>(size-offset, INT_MAX));
#ifdef _WIN32
        const int n = send(s, data+offset, amount, 0);
#else
        const int n = static_cast<int>(send(s, data+offset, static_cast<std::size_t>(amount), MSG_NOSIGNAL));
#endif
        if (n <= 0) return false;
        offset += static_cast<std::size_t>(n);
    }
    return true;
}
std::string lower(std::string s) {
    std::transform(s.begin(),s.end(),s.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    return s;
}
std::string statusText(int s) {
    switch(s) {
        case 200:return "OK"; case 202:return "Accepted"; case 204:return "No Content";
        case 400:return "Bad Request"; case 401:return "Unauthorized"; case 404:return "Not Found";
        case 405:return "Method Not Allowed"; case 409:return "Conflict"; case 413:return "Payload Too Large";
        case 415:return "Unsupported Media Type"; case 422:return "Unprocessable Entity";
        case 500:return "Internal Server Error"; case 503:return "Service Unavailable";
        case 504:return "Gateway Timeout"; default:return "Error";
    }
}
std::string jsonBody(const json& value) { return value.dump(); }
bool parseJson(const std::string& body, json& out) {
    try { out=json::parse(body); return out.is_object(); } catch(...) { return false; }
}
std::string safeFilename(std::string name) {
    const auto pos=name.find_last_of("/\\");
    if(pos!=std::string::npos) name=name.substr(pos+1);
    if(name.empty() || name=="." || name=="..") return "upload.bin";
    for(char& c:name) if(static_cast<unsigned char>(c)<32 || c==':' || c=='/' || c=='\\') c='_';
    if(name.size()>255) name.resize(255);
    return name;
}
bool parseMultipart(const std::string& body,const std::string& contentType,
                    std::string& recipient,std::string& filename,std::string& bytes) {
    const auto b=contentType.find("boundary=");
    if(b==std::string::npos) return false;
    std::string boundary=contentType.substr(b+9);
    if(!boundary.empty() && boundary.front()=='"') {
        const auto end=boundary.find('"',1); if(end==std::string::npos) return false;
        boundary=boundary.substr(1,end-1);
    } else {
        const auto semi=boundary.find(';'); if(semi!=std::string::npos) boundary.resize(semi);
    }
    if(boundary.empty() || boundary.size()>200) return false;
    const std::string marker="--"+boundary;
    std::size_t cursor=0;
    while((cursor=body.find(marker,cursor))!=std::string::npos) {
        cursor+=marker.size();
        if(body.compare(cursor,2,"--")==0) break;
        if(body.compare(cursor,2,"\r\n")!=0) return false;
        cursor+=2;
        const auto headerEnd=body.find("\r\n\r\n",cursor);
        if(headerEnd==std::string::npos) return false;
        const std::string headers=body.substr(cursor,headerEnd-cursor);
        const auto dataStart=headerEnd+4;
        const auto dataEnd=body.find("\r\n"+marker,dataStart);
        if(dataEnd==std::string::npos) return false;
        std::string field;
        const auto disposition=lower(headers).find("content-disposition:");
        if(disposition!=std::string::npos) {
            const auto lineEnd=headers.find("\r\n",disposition);
            const auto line=headers.substr(disposition,lineEnd==std::string::npos?std::string::npos:lineEnd-disposition);
            const auto n=line.find("name=\"");
            if(n!=std::string::npos) {
                const auto e=line.find('"',n+6);
                if(e!=std::string::npos) field=line.substr(n+6,e-(n+6));
            }
            const auto f=line.find("filename=\"");
            if(f!=std::string::npos) {
                const auto e=line.find('"',f+10);
                if(e!=std::string::npos) filename=safeFilename(line.substr(f+10,e-(f+10)));
            }
        }
        const std::string value=body.substr(dataStart,dataEnd-dataStart);
        if(field=="recipient") recipient=value;
        if(!filename.empty() && field=="file") bytes=value;
        cursor=dataEnd+2;
    }
    return !recipient.empty() && !filename.empty();
}
}

DcapHttpBridge::DcapHttpBridge(const std::string& address,std::uint16_t serverPort,std::uint16_t httpPort)
    : client_(address,serverPort),http_port_(httpPort) {}
DcapHttpBridge::~DcapHttpBridge(){ stop(); }

bool DcapHttpBridge::start() {
    if(running_) return true;
    if(!initializeSocketRuntime()) return false;
    listener_=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(listener_==INVALID_SOCKET_HANDLE) { cleanupSocketRuntime(); return false; }
    setReuseAddress(listener_);
    sockaddr_in local{}; local.sin_family=AF_INET;
    local.sin_addr.s_addr=htonl(INADDR_LOOPBACK); local.sin_port=htons(http_port_);
    if(::bind(listener_,reinterpret_cast<sockaddr*>(&local),sizeof(local))!=0 ||
       ::listen(listener_,SOMAXCONN)!=0) {
        closeSocket(listener_); listener_=INVALID_SOCKET_HANDLE; cleanupSocketRuntime(); return false;
    }
    running_=true;
    accept_thread_=std::thread(&DcapHttpBridge::acceptLoop,this);
    return true;
}
void DcapHttpBridge::acceptLoop() {
    while(running_) {
        SocketHandle s=::accept(listener_,nullptr,nullptr);
        if(s==INVALID_SOCKET_HANDLE) { if(!running_) break; continue; }
        std::lock_guard<std::mutex> lock(workers_mutex_);
        workers_.emplace_back(&DcapHttpBridge::handleConnection,this,s);
    }
}
void DcapHttpBridge::respond(SocketHandle s,int status,const std::string& type,
                             const std::string& body,const std::string& origin) {
    std::ostringstream out;
    out<<"HTTP/1.1 "<<status<<" "<<statusText(status)<<"\r\n"
       <<"Content-Type: "<<type<<"\r\nContent-Length: "<<body.size()
       <<"\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
    if(origin=="http://localhost" || origin=="http://127.0.0.1" ||
       origin.rfind("http://localhost:",0)==0 || origin.rfind("http://127.0.0.1:",0)==0) {
        out<<"Access-Control-Allow-Origin: "<<origin<<"\r\n"
           <<"Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
           <<"Access-Control-Allow-Headers: Content-Type\r\nVary: Origin\r\n";
    }
    out<<"\r\n";
    const auto header=out.str();
    sendBytes(s,header.data(),header.size());
    if(!body.empty()) sendBytes(s,body.data(),body.size());
}
void DcapHttpBridge::handleConnection(SocketHandle socket) {
#ifdef _WIN32
    DWORD receiveTimeout = 5000;
    setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<const char*>(&receiveTimeout),sizeof(receiveTimeout));
#else
    timeval receiveTimeout{5,0};
    setsockopt(socket,SOL_SOCKET,SO_RCVTIMEO,&receiveTimeout,sizeof(receiveTimeout));
#endif
    std::string request;
    std::vector<char> buffer(8192);
    std::size_t expected=0, headerEnd=std::string::npos, contentLength=0;
    std::string method,target,contentType,origin;
    while(request.size()<=MAX_HEADER+MAX_BODY) {
#ifdef _WIN32
        const int n=recv(socket,buffer.data(),static_cast<int>(buffer.size()),0);
#else
        const auto n=recv(socket,buffer.data(),buffer.size(),0);
#endif
        if(n<=0) break;
        request.append(buffer.data(),static_cast<std::size_t>(n));
        if(headerEnd==std::string::npos) {
            headerEnd=request.find("\r\n\r\n");
            if(headerEnd==std::string::npos) {
                if(request.size()>MAX_HEADER) break;
                continue;
            }
            std::istringstream headers(request.substr(0,headerEnd));
            std::string line; headers>>method>>target;
            std::getline(headers,line);
            while(std::getline(headers,line)) {
                if(!line.empty() && line.back()=='\r') line.pop_back();
                const auto colon=line.find(':'); if(colon==std::string::npos) continue;
                const auto name=lower(line.substr(0,colon));
                auto value=line.substr(colon+1);
                while(!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
                if(name=="content-length") {
                    try { contentLength=std::stoull(value); } catch(...) { contentLength=MAX_BODY+1; }
                } else if(name=="content-type") contentType=value;
                else if(name=="origin") origin=value;
            }
            if(contentLength>MAX_BODY) break;
            expected=headerEnd+4+contentLength;
        }
        if(expected && request.size()>=expected) { request.resize(expected); break; }
    }
    int status=400; json response={{"error","Malformed or incomplete HTTP request"}};
    std::string responseType="application/json";
    std::string rawResponse;
    bool sendRaw=false;
    if(expected && request.size()==expected) {
        const std::string body=request.substr(headerEnd+4);
        const auto query=target.find('?');
        const std::string path=target.substr(0,query);
        if(method=="OPTIONS") { status=204; response=json::object(); }
        else if(method=="GET" && (path=="/" || path=="/index.html" || path=="/app.js" || path=="/style.css")) {
            const std::string filename = path=="/" ? "index.html" : path.substr(1);
            auto filePath = std::filesystem::current_path() / "web" / filename;
            if(!std::filesystem::exists(filePath)) filePath = std::filesystem::current_path().parent_path() / "web" / filename;
            std::ifstream input(filePath,std::ios::binary);
            if(!input) { status=404; response={{"error","Web asset not found; run client_agent from the repository root"}}; }
            else {
                rawResponse.assign(std::istreambuf_iterator<char>(input),std::istreambuf_iterator<char>());
                responseType = filename=="index.html" ? "text/html; charset=utf-8" :
                               filename=="app.js" ? "application/javascript; charset=utf-8" : "text/css; charset=utf-8";
                status=200; sendRaw=true;
            }
        }
        else if(method=="POST" && path=="/signup") {
            json data;
            if(!parseJson(body,data) || !data.contains("username") || !data["username"].is_string() ||
               !data.contains("password") || !data["password"].is_string()) {
                status=400; response={{"error","Username and password are required"}};
            } else {
                std::lock_guard<std::mutex> lock(session_mutex_);
                const auto username=data["username"].get<std::string>();
                const auto password=data["password"].get<std::string>();
                if(client_.authenticated()) { status=409; response={{"error","Log out before creating another account"}}; }
                else if(password.size()<8 || password.size()>128) {
                    status=400; response={{"error","Password must contain 8 to 128 bytes"}};
                } else if(!client_.connect()) { status=503; response={{"error",client_.lastError()}}; }
                else if(!client_.registerAccount(username,password)) {
                    status=400; response={{"error","Invalid sign-up request"}};
                    client_.disconnect();
                } else {
                    bool done=false;
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
                    while(std::chrono::steady_clock::now()<deadline && !done) {
                        for(const auto& event:client_.takeEvents()) {
                            if(event.type=="SIGNUP_OK") {
                                status=200; response={{"created",true},{"message","Account created. You can now log in."}};
                                done=true; break;
                            }
                            if(event.type=="SIGNUP_FAIL") {
                                const auto code=event.data.value("code",0);
                                status=code==protocol::ERR_ACCOUNT_EXISTS?409:400;
                                response={{"error",event.data.value("error",std::string("Sign-up rejected"))},{"code",code}};
                                done=true; break;
                            }
                        }
                        if(!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if(!done) { status=504; response={{"error","Timed out waiting for account registration"}}; }
                    client_.disconnect();
                }
            }
        } else if(method=="POST" && path=="/login") {
            json data;
            if(!parseJson(body,data) || !data.contains("username") || !data["username"].is_string() ||
               !data.contains("password") || !data["password"].is_string()) {
                status=400; response={{"error","Username and password are required"}};
            } else {
                std::lock_guard<std::mutex> lock(session_mutex_);
                if(client_.authenticated()) { status=409; response={{"error","Already logged in"}}; }
                else if(!client_.connect()) { status=503; response={{"error",client_.lastError()}}; }
                else if(!client_.login(data["username"].get<std::string>(),
                                       data["password"].get<std::string>())) {
                    status=400; response={{"error","Invalid login request"}};
                    client_.disconnect();
                } else {
                    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
                    bool done=false;
                    while(std::chrono::steady_clock::now()<deadline && !done) {
                        for(auto& event:client_.takeEvents()) {
                            if(event.type=="LOGIN_OK") { done=true; break; }
                            if(event.type=="LOGIN_FAIL") {
                                const auto code=event.data.value("code",0);
                                status=code==protocol::ERR_ACCOUNT_ALREADY_ONLINE?409:401;
                                response={{"error",event.data.value("error",std::string("Login rejected"))},{"code",code}};
                                done=true; break;
                            }
                            std::lock_guard<std::mutex> q(events_mutex_);
                            pending_events_.push_back(std::move(event));
                        }
                        if(!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    }
                    if(status==400 && !done) {
                        status=504; response={{"error","Timed out waiting for authentication"}};
                        client_.disconnect();
                    } else if(status==400) {
                        status=200; response={{"authenticated",true},{"username",client_.username()}};
                    }
                }
            }
        } else if(method=="POST" && path=="/logout") {
            std::lock_guard<std::mutex> lock(session_mutex_); client_.logout();
            status=200; response={{"authenticated",false}};
        } else if(method=="POST" && (path=="/global_message" || path=="/private_message")) {
            json data;
            if(!parseJson(body,data)) { status=400; response={{"error","Invalid JSON body"}}; }
            else if(!client_.authenticated()) { status=401; response={{"error","Authentication required"}}; }
            else {
                const auto content=data.value("content",std::string());
                bool sent=false;
                if(path=="/global_message") sent=client_.sendGlobal(content);
                else sent=client_.sendPrivate(data.value("recipient",std::string()),content);
                status=sent?202:422; response=sent?json{{"status","accepted"}}:json{{"error","Invalid message or recipient"}};
            }
        } else if(method=="GET" && path=="/user_list") {
            if(!client_.authenticated()) { status=401; response={{"error","Authentication required"}}; }
            else { status=200; response={{"users",client_.users()}}; }
        } else if(method=="GET" && path=="/events") {
            if(!client_.authenticated()) { status=401; response={{"error","Authentication required"}}; }
            else {
                json events=json::array();
                {
                    std::lock_guard<std::mutex> lock(events_mutex_);
                    for(auto& e:pending_events_) events.push_back({{"type",e.type},{"data",e.data}});
                    pending_events_.clear();
                }
                for(auto& e:client_.takeEvents()) events.push_back({{"type",e.type},{"data",e.data}});
                status=200; response={{"events",events}};
            }
        } else if(method=="GET" && path=="/status") {
            status=200; response={{"connected",client_.connected()},{"authenticated",client_.authenticated()},
                {"username",client_.username()},{"server_port",server_port_},{"last_connection_error",client_.lastError()}};
        } else if(method=="POST" && path=="/file_decision") {
            json data;
            if(!parseJson(body,data) || !data.contains("transfer_id") || !data.contains("accept")) {
                status=400; response={{"error","transfer_id and accept are required"}};
            } else {
                const bool ok=client_.decideFile(data["transfer_id"].get<std::string>(),data["accept"].get<bool>());
                status=ok?202:404; response=ok?json{{"status","accepted"}}:json{{"error","Unknown transfer offer"}};
            }
        } else if(method=="POST" && path=="/file_upload") {
            if(!client_.authenticated()) { status=401; response={{"error","Authentication required"}}; }
            else {
                std::string recipient,filename,fileBytes;
                if(!parseMultipart(body,contentType,recipient,filename,fileBytes)) {
                    status=400; response={{"error","Expected multipart fields recipient and file"}};
                } else {
                    try {
                        const auto directory=std::filesystem::current_path()/"uploads";
                        std::filesystem::create_directories(directory);
                        const auto path=directory/(std::to_string(nowMs())+"-"+filename);
                        std::ofstream out(path,std::ios::binary);
                        out.write(fileBytes.data(),static_cast<std::streamsize>(fileBytes.size()));
                        out.close();
                        if(!out) throw std::runtime_error("Could not save uploaded file");
                        const auto id=client_.sendFile(recipient,path);
                        if(id.empty()) { status=422; response={{"error",client_.lastError().empty()?"Could not start file transfer":client_.lastError()}}; }
                        else { status=202; response={{"transfer_id",id},{"status","offered"}}; }
                    } catch(const std::exception& e) { status=500; response={{"error",e.what()}}; }
                }
            }
        } else { status=404; response={{"error","Endpoint not found"}}; }
    }
    respond(socket,status,responseType,sendRaw?rawResponse:(status==204?std::string():jsonBody(response)),origin);
    shutdownSocket(socket); closeSocket(socket);
}
void DcapHttpBridge::stop() {
    if(!running_.exchange(false)) return;
    if(listener_!=INVALID_SOCKET_HANDLE) { shutdownSocket(listener_); closeSocket(listener_); listener_=INVALID_SOCKET_HANDLE; }
    if(accept_thread_.joinable()) accept_thread_.join();
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for(auto& worker:workers_) if(worker.joinable()) worker.join();
        workers_.clear();
    }
    client_.disconnect();
    cleanupSocketRuntime();
}
