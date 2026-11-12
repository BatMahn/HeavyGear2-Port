#include "net.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace hg2 {

namespace {
constexpr uint32_t kMagic = 0x4e324748;  // "HG2N"
constexpr uint8_t kVersion = 1;

#ifdef _WIN32
struct WsaInit {
    bool ok = false;
    WsaInit() {
        WSADATA w;
        ok = WSAStartup(MAKEWORD(2, 2), &w) == 0;
    }
    ~WsaInit() {
        if (ok) WSACleanup();
    }
};
void ensureWsa() { static WsaInit w; }
#else
void ensureWsa() {}
#endif

// packet writer / reader (little endian)
struct W {
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint16_t v) { u8(uint8_t(v)), u8(uint8_t(v >> 8)); }
    void u32(uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); }
    void i32(int32_t v) { u32(uint32_t(v)); }
    void f32(float v) {
        uint32_t u;
        memcpy(&u, &v, 4);
        u32(u);
    }
    void vec(const Vec3& v) { f32(v.x), f32(v.y), f32(v.z); }
    void str(const std::string& s) {
        u8(uint8_t(std::min<size_t>(s.size(), 255)));
        b.insert(b.end(), s.begin(), s.begin() + long(std::min<size_t>(s.size(), 255)));
    }
    void gear(const NetGear& g) {
        i32(g.chassis), i32(g.armor), i32(g.fireControl);
        u8(uint8_t(std::min<size_t>(g.weapons.size(), 16)));
        for (size_t i = 0; i < g.weapons.size() && i < 16; i++) u8(uint8_t(g.weapons[i].first)), u16(uint16_t(g.weapons[i].second));
    }
};
struct R {
    const uint8_t* p;
    size_t n, i = 0;
    bool bad = false;
    R(const uint8_t* d, size_t len) : p(d), n(len) {}
    uint8_t u8() {
        if (i >= n) return bad = true, 0;
        return p[i++];
    }
    uint16_t u16() { uint16_t a = u8(); return uint16_t(a | u8() << 8); }
    uint32_t u32() { uint32_t a = u16(); return a | uint32_t(u16()) << 16; }
    int32_t i32() { return int32_t(u32()); }
    float f32() {
        uint32_t u = u32();
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    Vec3 vec() { float x = f32(), y = f32(); return Vec3(x, y, f32()); }
    std::string str() {
        size_t l = u8();
        if (i + l > n) return bad = true, std::string();
        std::string s(reinterpret_cast<const char*>(p + i), l);
        i += l;
        return s;
    }
    NetGear gear() {
        NetGear g;
        g.chassis = i32(), g.armor = i32(), g.fireControl = i32();
        int k = u8();
        for (int j = 0; j < k && !bad; j++) {
            int slot = u8();
            g.weapons.push_back({slot, int(u16())});
        }
        return g;
    }
};
W header(uint8_t type, int sender) {
    W w;
    w.u32(kMagic), w.u8(kVersion), w.u8(type), w.u8(uint8_t(sender));
    return w;
}
uint64_t nowMs() { return SDL_GetTicks(); }
}  // namespace

// ---------------------------------------------------------------- socket
UdpSocket::~UdpSocket() { close(); }

bool UdpSocket::open(uint16_t port) {
    ensureWsa();
    close();
    intptr_t s = intptr_t(socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (s < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(port), a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(decltype(socket(0, 0, 0))(s), reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
#ifdef _WIN32
        closesocket(SOCKET(s));
#else
        ::close(int(s));
#endif
        return false;
    }
#ifdef _WIN32
    u_long nb = 1;
    ioctlsocket(SOCKET(s), FIONBIO, &nb);
#else
    fcntl(int(s), F_SETFL, fcntl(int(s), F_GETFL, 0) | O_NONBLOCK);
#endif
    fd_ = s;
    return true;
}

void UdpSocket::close() {
    if (fd_ < 0) return;
#ifdef _WIN32
    closesocket(SOCKET(fd_));
#else
    ::close(int(fd_));
#endif
    fd_ = -1;
}

bool UdpSocket::sendTo(uint32_t ip, uint16_t port, const void* data, size_t n) {
    if (fd_ < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET, a.sin_port = htons(port), a.sin_addr.s_addr = ip;
    return sendto(decltype(socket(0, 0, 0))(fd_), reinterpret_cast<const char*>(data), int(n), 0, reinterpret_cast<sockaddr*>(&a), sizeof a) ==
           int(n);
}

bool UdpSocket::sendTo(const std::string& host, uint16_t port, const void* data, size_t n) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) return false;
    uint32_t ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(res);
    return sendTo(ip, port, data, n);
}

bool UdpSocket::recv(std::vector<uint8_t>& out, uint32_t& ip, uint16_t& port) {
    if (fd_ < 0) return false;
    uint8_t buf[2048];
    sockaddr_in a{};
    socklen_t len = sizeof a;
    auto n = recvfrom(decltype(socket(0, 0, 0))(fd_), reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&a), &len);
    if (n <= 0) return false;
    out.assign(buf, buf + n);
    ip = a.sin_addr.s_addr, port = ntohs(a.sin_port);
    return true;
}

// ---------------------------------------------------------------- game
const NetPlayer* NetGame::player(int id) const {
    for (auto& p : players_)
        if (p.id == id) return &p;
    return nullptr;
}

bool NetGame::host(const std::string& name, const NetGear& gear, uint16_t port) {
    stop();
    if (!sock_.open(port)) {
        status = "Could not open UDP port " + std::to_string(port) + ".";
        return false;
    }
    isHost_ = true, myId_ = 0, phase_ = Phase::Lobby, nextId_ = 1;
    myName_ = name, myGear_ = gear;
    players_.push_back({0, name, gear});
    seed = uint32_t(SDL_GetTicks()) | 1;
    status = "Hosting on port " + std::to_string(port) + ". Waiting for players.";
    startWorker();
    return true;
}

bool NetGame::join(const std::string& address, const std::string& name, const NetGear& gear) {
    stop();
    std::string h = address;
    uint16_t port = kPort;
    size_t c = h.rfind(':');
    if (c != std::string::npos) port = uint16_t(atoi(h.c_str() + c + 1)), h = h.substr(0, c);
    if (!sock_.open(0)) {
        status = "Could not open a UDP socket.";
        return false;
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET, hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(h.c_str(), nullptr, &hints, &res) != 0 || !res) {
        status = "Unknown host " + h + ".";
        sock_.close();
        return false;
    }
    hostIp_ = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr, hostPort_ = port;
    freeaddrinfo(res);
    isHost_ = false, myId_ = -1, phase_ = Phase::Lobby;
    myName_ = name, myGear_ = gear;
    lastHeard_ = nowMs(), lastHello_ = 0;
    status = "Contacting " + address + "...";
    lastRx_ = nowMs();
    startWorker();
    return true;
}

void NetGame::stop() {
    if (sock_.isOpen() && phase_ != Phase::Idle) {
        W w = header(LEAVE, myId_ < 0 ? 255 : myId_);
        send(w.b);
    }
    stopWorker();
    sock_.close();
    players_.clear();
    phase_ = Phase::Idle, myId_ = -1, isHost_ = false;
    states.clear(), fires.clear(), deaths.clear(), left.clear();
    startReceived = endReceived = false;
}

void NetGame::setMyGear(const NetGear& g) {
    myGear_ = g;
    for (auto& p : players_)
        if (p.id == myId_) p.gear = g;
    lastHello_ = 0;  // client: resend
}

void NetGame::send(const std::vector<uint8_t>& pkt) {
    if (isHost_) {
        for (auto& p : players_)
            if (p.id != 0 && p.ip) sock_.sendTo(p.ip, p.port, pkt.data(), pkt.size());
    } else if (hostIp_) {
        sock_.sendTo(hostIp_, hostPort_, pkt.data(), pkt.size());
    }
}

void NetGame::sendExcept(const std::vector<uint8_t>& pkt, int exceptId) {
    for (auto& p : players_)
        if (p.id != 0 && p.id != exceptId && p.ip) sock_.sendTo(p.ip, p.port, pkt.data(), pkt.size());
}

void NetGame::lobbyPacket(std::vector<uint8_t>& out) const {
    W w = header(LOBBY, 0);
    w.str(arena), w.i32(fragLimit), w.i32(timeLimit), w.u32(seed);
    w.u8(uint8_t(players_.size()));
    for (auto& p : players_) {
        w.u8(uint8_t(p.id)), w.str(p.name), w.gear(p.gear);
        w.i32(p.kills), w.i32(p.deaths);
    }
    out = w.b;
}

void NetGame::readLobby(const uint8_t* p, size_t n) {
    R r(p, n);
    arena = r.str(), fragLimit = r.i32(), timeLimit = r.i32(), seed = r.u32();
    int k = r.u8();
    std::vector<NetPlayer> ps;
    for (int i = 0; i < k && !r.bad; i++) {
        NetPlayer q;
        q.id = r.u8(), q.name = r.str(), q.gear = r.gear();
        q.kills = r.i32(), q.deaths = r.i32();
        ps.push_back(q);
    }
    if (!r.bad) players_ = ps;
}

void NetGame::start() {
    if (!isHost_) return;
    std::vector<uint8_t> lob;
    lobbyPacket(lob);
    W w = header(START, 0);
    w.b.insert(w.b.end(), lob.begin() + 7, lob.end());  // the lobby payload: settings and players
    for (int i = 0; i < 3; i++) send(w.b);              // UDP: say it a few times
    phase_ = Phase::Playing;
    startReceived = true;
}

void NetGame::handle(const std::vector<uint8_t>& pkt, uint32_t ip, uint16_t port) {
    if (getenv("HG2_DEBUG") && pkt.size() > 5) SDL_Log("net: packet type %u, %zu bytes from %08x:%u", pkt[5], pkt.size(), ip, port);
    if (pkt.size() < 7) return;
    R h(pkt.data(), pkt.size());
    if (h.u32() != kMagic || h.u8() != kVersion) return;
    uint8_t type = h.u8();
    int sender = h.u8();
    const uint8_t* body = pkt.data() + 7;
    size_t bn = pkt.size() - 7;
    if (isHost_) {
        NetPlayer* from = nullptr;
        for (auto& p : players_)
            if (p.ip == ip && p.port == port && p.id != 0) from = &p;
        if (type == HELLO) {
            R r(body, bn);
            std::string name = r.str();
            NetGear g = r.gear();
            if (r.bad) return;
            if (!from) {
                if (phase_ != Phase::Lobby || players_.size() >= 8) return;
                players_.push_back({nextId_++, name, g, ip, port});
                from = &players_.back();
                status = name + " joined.";
            }
            from->name = name, from->gear = g, from->lastHeard = nowMs();
            W w = header(WELCOME, 0);
            w.u8(uint8_t(from->id));
            sock_.sendTo(ip, port, w.b.data(), w.b.size());
            return;
        }
        if (!from) return;
        from->lastHeard = nowMs();
        if (type == LEAVE) {
            int id = from->id;
            status = from->name + " left.";
            players_.erase(players_.begin() + (from - players_.data()));
            left.push_back(id);
            W w = header(LEAVE, id);
            send(w.b);
            return;
        }
        if (type == STATE || type == FIRE || type == DEATH) {
            // relay to the others with the sender's real id, then handle it here too
            std::vector<uint8_t> fwd = pkt;
            fwd[6] = uint8_t(from->id);
            sendExcept(fwd, from->id);
            sender = from->id;
        }
    } else {
        lastHeard_ = nowMs();
        if (type == WELCOME) {
            R r(body, bn);
            int id = r.u8();
            if (!r.bad && myId_ != id) myId_ = id, status = "Joined. Waiting for the host to start.";
            return;
        }
        if (type == LOBBY) {
            readLobby(body, bn);
            return;
        }
        if (type == START && phase_ == Phase::Lobby) {
            readLobby(body, bn);
            phase_ = Phase::Playing;
            startReceived = true;
            return;
        }
        if (type == END) {
            endReceived = true, phase_ = Phase::Ended;
            return;
        }
        if (type == LEAVE) {
            for (size_t i = 0; i < players_.size(); i++)
                if (players_[i].id == sender) players_.erase(players_.begin() + long(i)), left.push_back(sender);
            if (sender == 0) status = "The host left.", endReceived = true;
            return;
        }
    }
    R r(body, bn);
    if (type == STATE) {
        NetState s;
        s.id = sender, s.seq = r.u32(), s.pos = r.vec(), s.yaw = r.f32(), s.twist = r.f32();
        s.throttle = r.f32(), s.turn = r.f32(), s.strafe = r.f32();
        s.run = r.u8(), s.posture = r.u8(), s.skates = r.u8(), s.airborne = r.u8(), s.alive = r.u8();
        s.hp = r.f32();
        if (!r.bad && sender != myId_) states.push_back(s);
    } else if (type == FIRE) {
        NetFire f{sender, r.vec(), Vec3()};
        f.aim = r.vec();
        if (!r.bad && sender != myId_) fires.push_back(f);
    } else if (type == DEATH) {
        NetDeath d{sender, r.i32()};
        if (!r.bad) {
            deaths.push_back(d);
            for (auto& p : players_) {
                if (p.id == d.victim) p.deaths++;
                if (p.id == d.killer && d.killer != d.victim) p.kills++;
            }
        }
    }
}

void NetGame::startWorker() {
    stopWorker();
    running_ = true;
    worker_ = std::thread([this]() {
        std::vector<uint8_t> pkt;
        uint32_t ip;
        uint16_t port;
        uint64_t lastKeep = 0;
        while (running_) {
            bool got = false;
            for (int i = 0; i < 256 && sock_.recv(pkt, ip, port); i++) {
                std::lock_guard<std::mutex> g(mx_);
                if (inbox_.size() < 4096) inbox_.push_back({pkt, ip, port});
                lastRx_ = nowMs(), got = true;
            }
            uint64_t t = nowMs();
            if (t - lastKeep >= keepaliveMs_) {
                std::lock_guard<std::mutex> g(mx_);
                if (!keepalive_.empty())
                    for (auto& [tip, tport] : targets_) sock_.sendTo(tip, tport, keepalive_.data(), keepalive_.size());
                lastKeep = t;
            }
            if (!got) SDL_Delay(4);
        }
    });
}

void NetGame::stopWorker() {
    running_ = false;
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> g(mx_);
    inbox_.clear(), keepalive_.clear(), targets_.clear();
}

void NetGame::setKeepalive(const std::vector<uint8_t>& pkt, uint32_t intervalMs) {
    std::vector<std::pair<uint32_t, uint16_t>> t;
    if (isHost_) {
        for (auto& p : players_)
            if (p.id != 0 && p.ip) t.push_back({p.ip, p.port});
    } else if (hostIp_) {
        t.push_back({hostIp_, hostPort_});
    }
    std::lock_guard<std::mutex> g(mx_);
    keepalive_ = pkt, keepaliveMs_ = intervalMs, targets_ = t;
}

bool NetGame::poll() {
    if (!sock_.isOpen()) return false;
    std::deque<Inbound> in;
    {
        std::lock_guard<std::mutex> g(mx_);
        in.swap(inbox_);
    }
    for (auto& p : in) handle(p.pkt, p.ip, p.port);
    if (!in.empty() && !isHost_) lastHeard_ = std::max<uint64_t>(lastHeard_, lastRx_);
    uint64_t t = nowMs();
    // what the worker repeats while this thread may be busy: the lobby (host), our introduction (client in the
    // lobby), or a ping (in a match)
    if (isHost_) {
        std::vector<uint8_t> k;
        if (phase_ == Phase::Lobby) lobbyPacket(k);
        else k = header(PING, 0).b;
        setKeepalive(k, phase_ == Phase::Lobby ? 500 : 1000);
        // drop clients not heard from for 30 s
        for (size_t i = 1; i < players_.size();)
            if (players_[i].lastHeard && t - players_[i].lastHeard > 30000) {
                status = players_[i].name + " timed out.";
                left.push_back(players_[i].id);
                players_.erase(players_.begin() + long(i));
            } else {
                i++;
            }
        return true;
    }
    if (phase_ == Phase::Lobby) {
        W w = header(HELLO, myId_ < 0 ? 255 : myId_);
        w.str(myName_), w.gear(myGear_);
        setKeepalive(w.b, 1000);
    } else {
        setKeepalive(header(PING, myId_).b, 1000);
    }
    if (t - std::max<uint64_t>(lastHeard_, lastRx_) > 30000) {
        status = "Lost the connection to the host.";
        return false;
    }
    return true;
}

void NetGame::sendState(const NetState& s) {
    W w = header(STATE, myId_);
    w.u32(s.seq), w.vec(s.pos), w.f32(s.yaw), w.f32(s.twist), w.f32(s.throttle), w.f32(s.turn), w.f32(s.strafe);
    w.u8(s.run), w.u8(s.posture), w.u8(s.skates), w.u8(s.airborne), w.u8(s.alive);
    w.f32(s.hp);
    send(w.b);
    lastSent_ = nowMs();
}

void NetGame::sendFire(const Vec3& muzzle, const Vec3& aim) {
    W w = header(FIRE, myId_);
    w.vec(muzzle), w.vec(aim);
    send(w.b);
}

void NetGame::sendDeath(int killer) {
    W w = header(DEATH, myId_);
    w.i32(killer);
    send(w.b);
    deaths.push_back({myId_, killer});
    for (auto& p : players_) {
        if (p.id == myId_) p.deaths++;
        if (p.id == killer && killer != myId_) p.kills++;
    }
}

void NetGame::endMatch() {
    if (!isHost_) return;
    W w = header(END, 0);
    for (int i = 0; i < 3; i++) send(w.b);
    phase_ = Phase::Ended, endReceived = true;
}

}  // namespace hg2
