// Network play for the port: a small UDP protocol of its own (the original's ANet / DirectPlay sessions are not
// compatible with anything still around). One player hosts, the others join by address; the host relays every
// packet, so clients only talk to the host.
//
// Each machine simulates its own gear and sends its state 20 times a second; the other gears are drawn from those
// states. Shots are sent as they're fired and replayed on every machine; a gear only takes damage on its owner's
// machine (from the replayed shots there), so every player is the authority on their own armour. Deaths carry the
// killer for the frag count.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <string>
#include <vector>

#include "../core/math.h"

namespace hg2 {

class UdpSocket {
public:
    ~UdpSocket();
    bool open(uint16_t port);  // 0: any port
    void close();
    bool sendTo(const std::string& host, uint16_t port, const void* data, size_t n);
    bool sendTo(uint32_t ip, uint16_t port, const void* data, size_t n);  // ip in network order
    // Non-blocking receive; false when nothing is waiting.
    bool recv(std::vector<uint8_t>& out, uint32_t& ip, uint16_t& port);
    bool isOpen() const { return fd_ >= 0; }

private:
    intptr_t fd_ = -1;
};

struct NetGear {           // a player's gear as the shell chose it
    int chassis = -1;
    std::vector<std::pair<int, int>> weapons;
    int armor = 0, fireControl = 0;
};
struct NetPlayer {
    int id = -1;           // 0 = host
    std::string name;
    NetGear gear;
    uint32_t ip = 0;       // host side: where the player is
    uint16_t port = 0;
    uint64_t lastHeard = 0;  // ticks (ms)
    int kills = 0, deaths = 0;
};
struct NetState {          // one gear's state
    int id = -1;
    uint32_t seq = 0;
    Vec3 pos;
    float yaw = 0, twist = 0, throttle = 0, turn = 0, strafe = 0;
    uint8_t run = 0, posture = 1, skates = 0, airborne = 0, alive = 1;
    float hp = 1;          // 0..1
};
struct NetFire { int id; Vec3 muzzle, aim; };
struct NetDeath { int victim, killer; };

class NetGame {
public:
    static constexpr uint16_t kPort = 47250;
    enum class Phase { Idle, Lobby, Playing, Ended };

    ~NetGame() { stop(); }
    bool host(const std::string& name, const NetGear& gear, uint16_t port = kPort);
    bool join(const std::string& address, const std::string& name, const NetGear& gear);  // "host[:port]"
    void stop();
    bool isHost() const { return isHost_; }
    int myId() const { return myId_; }
    Phase phase() const { return phase_; }
    const std::vector<NetPlayer>& players() const { return players_; }
    const NetPlayer* player(int id) const;
    std::string status;     // last lobby message for the screen

    // lobby (host): the match settings, sent with the player list; start() begins the match everywhere
    std::string arena = "D01";
    int fragLimit = 10, timeLimit = 600;  // seconds
    uint32_t seed = 1;
    void setMyGear(const NetGear& g);
    void start();

    // Called every frame: reads packets, keeps the lobby alive, relays (host). Returns false if the connection
    // dropped (client: nothing from the host for 30 s).
    bool poll();

    // in a match
    void sendState(const NetState& s);
    void sendFire(const Vec3& muzzle, const Vec3& aim);
    void sendDeath(int killer);
    void endMatch();                  // host: the match is over
    std::vector<NetState> states;     // received since the last call (consumed by the game)
    std::vector<NetFire> fires;
    std::vector<NetDeath> deaths;
    std::vector<int> left;            // players who left
    bool startReceived = false, endReceived = false;

private:
    enum Type : uint8_t { HELLO = 1, WELCOME, LOBBY, START, STATE, FIRE, DEATH, LEAVE, END, PING };
    void send(const std::vector<uint8_t>& pkt);  // client: to host; host: to every client
    void sendExcept(const std::vector<uint8_t>& pkt, int exceptId);
    void lobbyPacket(std::vector<uint8_t>& out) const;
    void readLobby(const uint8_t* p, size_t n);
    void handle(const std::vector<uint8_t>& pkt, uint32_t ip, uint16_t port);

    // A worker thread reads the socket and sends the keep-alive packet (lobby / hello / ping) on its own, so a
    // stalled frame (an arena loading, a hidden window) doesn't drop anyone; packets are handled in poll().
    void startWorker();
    void stopWorker();
    void setKeepalive(const std::vector<uint8_t>& pkt, uint32_t intervalMs);
    struct Inbound { std::vector<uint8_t> pkt; uint32_t ip; uint16_t port; };
    std::thread worker_;
    std::mutex mx_;
    std::deque<Inbound> inbox_;
    std::vector<uint8_t> keepalive_;
    uint32_t keepaliveMs_ = 1000;
    std::vector<std::pair<uint32_t, uint16_t>> targets_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastRx_{0};
    UdpSocket sock_;
    bool isHost_ = false;
    int myId_ = -1;
    Phase phase_ = Phase::Idle;
    std::vector<NetPlayer> players_;
    std::string hostName_;
    uint32_t hostIp_ = 0;
    uint16_t hostPort_ = 0;
    uint64_t lastSent_ = 0, lastHeard_ = 0, lastHello_ = 0;
    std::string myName_;
    NetGear myGear_;
    int nextId_ = 1;
};

}  // namespace hg2
