// Protocol, framing, state & IPC tests for bose-700-daemon.
// Test harness style adapted from omarchy-sony-xm3 (Copyright (c) Kevin
// Cardwell, MIT license). BMAP expectations are taken from bosectl
// (Copyright (c) aaronsb, MIT license) and from captures verified against a
// real NC700 running firmware 1.8.2-11524+e0f7590.

#include "BmapProtocol.hpp"
#include "BluetoothManager.hpp"
#include "BluezWatcher.hpp"
#include "BmapLink.hpp"
#include "StateEngine.hpp"
#include "IpcServer.hpp"

#include <iostream>
#include <fstream>
#include <filesystem>
#include <vector>
#include <string>
#include <cassert>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/poll.h>
#include <thread>
#include <chrono>
#include <atomic>

using namespace omarchy::bose;

static int gFailedTests = 0;
static int gTotalTests = 0;

#define TEST_CASE(name) \
    std::cout << "[ RUN      ] " << name << std::endl; \
    gTotalTests++;

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[  FAILED  ] " << msg << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_ASSERT_EQ(actual, expected, msg) \
    do { \
        if ((actual) != (expected)) { \
            std::cerr << "[  FAILED  ] " << msg << " | Expected: " << (expected) \
                      << ", Actual: " << (actual) << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; \
            gFailedTests++; \
            return; \
        } \
    } while (0)

#define TEST_PASS(name) \
    std::cout << "[       OK ] " << name << std::endl;

static std::vector<uint8_t> V(std::initializer_list<int> bytes) {
    std::vector<uint8_t> out;
    for (int b : bytes) out.push_back(static_cast<uint8_t>(b));
    return out;
}

// ---------------------------------------------------------------------------
// 1. BMAP Packet Build / Parse Round-Trips
// ---------------------------------------------------------------------------
void testPacketRoundTrip() {
    TEST_CASE("PacketRoundTrip");

    // GET [1.5] -> 01 05 01 00
    auto pkt = bmap::bmap_packet(1, 5, bmap::Operator::Get);
    TEST_ASSERT_EQ(pkt.size(), size_t(4), "empty GET is 4 bytes");
    TEST_ASSERT_EQ(pkt[0], 0x01, "fblock");
    TEST_ASSERT_EQ(pkt[1], 0x05, "func");
    TEST_ASSERT_EQ(pkt[2], 0x01, "GET operator");
    TEST_ASSERT_EQ(pkt[3], 0x00, "zero payload length");

    auto resp = bmap::parse_response(pkt);
    TEST_ASSERT(resp.has_value(), "parse own packet");
    TEST_ASSERT_EQ(resp->fblock, 1, "fblock round-trip");
    TEST_ASSERT(resp->op == bmap::Operator::Get, "operator round-trip");
    TEST_ASSERT(resp->payload.empty(), "empty payload round-trip");

    // SETGET [1.5] — strength 8 -> raw {2, 1} (wire axis inverted: raw 0 = max ANC)
    auto set = bmap::bmap_packet(1, 5, bmap::Operator::SetGet, bmap::build_cnc(8));
    TEST_ASSERT_EQ(set.size(), size_t(6), "SETGET cnc is 6 bytes");
    TEST_ASSERT_EQ(set[2], 0x02, "SETGET operator");
    TEST_ASSERT_EQ(set[3], 0x02, "payload length 2");
    TEST_ASSERT_EQ(set[4], 0x02, "cnc raw level (10 - strength)");
    TEST_ASSERT_EQ(set[5], 0x01, "cnc trailer");

    auto setResp = bmap::parse_response(set);
    TEST_ASSERT(setResp.has_value() && setResp->payload.size() == 2, "payload round-trip");
    TEST_ASSERT_EQ(setResp->payload[0], 2, "payload byte 0 (raw = 10 - strength)");

    // STATUS [2.2] {0x58} battery reply
    auto status = bmap::bmap_packet(2, 2, bmap::Operator::Status, {0x58});
    auto parsed = bmap::parse_response(status);
    TEST_ASSERT(parsed.has_value() && parsed->op == bmap::Operator::Status, "status op");
    TEST_ASSERT_EQ(bmap::parse_battery(parsed->payload), 88, "battery 0x58 = 88%");

    // Truncated buffer is rejected
    TEST_ASSERT(!bmap::parse_response(V({1, 5, 3})).has_value(), "short buffer rejected");

    // ERROR packet carries the error code
    auto errPkt = bmap::bmap_packet(31, 1, bmap::Operator::Error, {3});
    auto errParsed = bmap::parse_response(errPkt);
    TEST_ASSERT(errParsed.has_value() && errParsed->op == bmap::Operator::Error, "error op");
    TEST_ASSERT_EQ(errParsed->payload[0], 3, "FblockNotSupp code");
    TEST_ASSERT(errParsed->fmt().find("FblockNotSupp") != std::string::npos, "error name in fmt");

    TEST_PASS("PacketRoundTrip");
}

// ---------------------------------------------------------------------------
// 2. Stream Framer
// ---------------------------------------------------------------------------
void testStreamParser() {
    TEST_CASE("StreamParser");

    bmap::StreamParser p;

    // Partial feed: nothing until the packet is complete
    auto full = bmap::bmap_packet(2, 2, bmap::Operator::Status, {0x58});
    p.feed(full.data(), 2);
    TEST_ASSERT(!p.next().has_value(), "no packet from 2 bytes");
    p.feed(full.data() + 2, full.size() - 2);
    auto pkt = p.next();
    TEST_ASSERT(pkt.has_value(), "packet completes across feeds");
    TEST_ASSERT_EQ(pkt->fblock, 2, "framed fblock");
    TEST_ASSERT_EQ(bmap::parse_battery(pkt->payload), 88, "framed payload");
    TEST_ASSERT(!p.next().has_value(), "no more packets");

    // Two packets in one feed come out in order
    auto a = bmap::bmap_packet(1, 5, bmap::Operator::Status, {0x0b, 0x00, 0x01});
    auto b = bmap::bmap_packet(1, 11, bmap::Operator::Status, {0x01, 0x02, 0x0f});
    std::vector<uint8_t> both(a.begin(), a.end());
    both.insert(both.end(), b.begin(), b.end());
    p.feed(both.data(), both.size());
    auto first = p.next();
    auto second = p.next();
    TEST_ASSERT(first.has_value() && second.has_value(), "two packets parsed");
    TEST_ASSERT_EQ(first->func, 5, "first is cnc");
    TEST_ASSERT_EQ(second->func, 11, "second is sidetone");
    TEST_ASSERT(!p.next().has_value(), "stream drained");

    // Trailing garbage shorter than a header is kept, then reset clears it
    const auto garbage = V({0x99});
    p.feed(garbage.data(), garbage.size());
    TEST_ASSERT_EQ(p.buffered(), size_t(1), "garbage buffered");
    p.reset();
    TEST_ASSERT_EQ(p.buffered(), size_t(0), "reset clears");

    TEST_PASS("StreamParser");
}

// ---------------------------------------------------------------------------
// 3. Field Parsers (verified NC700 captures)
// ---------------------------------------------------------------------------
void testFieldParsers() {
    TEST_CASE("FieldParsers");

    // [0.5] firmware: ASCII
    std::vector<uint8_t> fw = {'1', '.', '8', '.', '2', '-', '1', '1', '5', '2', '4', '+', 'e', '0', 'f', '7', '5', '9', '0'};
    TEST_ASSERT(bmap::parse_firmware(fw) == "1.8.2-11524+e0f7590", "firmware string");

    // [1.2] name: leading flag byte, UTF-8 name
    auto nameBytes = V({0x00, 'P', 'a', 'n', 't', 'h', 0xC3, 0xA8, 'r', 'e'});
    TEST_ASSERT(bmap::parse_product_name(nameBytes) == "Panthère", "name skips flag byte");
    TEST_ASSERT(bmap::parse_product_name(V({0x00})).empty(), "name with only flag is empty");

    // [1.5] cnc: 0b 00 01 -> raw current 0 (max ANC), max 10 -> strength 10
    auto cnc = bmap::parse_cnc(V({0x0b, 0x00, 0x01}));
    TEST_ASSERT_EQ(cnc.first, 10, "cnc strength (raw 0 = max ANC)");
    TEST_ASSERT_EQ(cnc.second, 10, "cnc max");
    // raw 10 = full passthrough -> strength 0
    TEST_ASSERT_EQ(bmap::parse_cnc(V({0x0b, 0x0a, 0x01})).first, 0, "cnc raw 10 = passthrough");

    // [1.7] eq: three [min, max, current, band_id] groups, range -10..+10
    auto eq = bmap::parse_eq(V({
        0xf6, 0x0a, 0x00, 0x00,   // bass: min -10, max 10, current 0, id 0
        0xf6, 0x0a, 0xfd, 0x01,   // mid: current -3, id 1
        0xf6, 0x0a, 0x05, 0x02,   // treble: current +5, id 2
    }));
    TEST_ASSERT_EQ(eq.size(), size_t(3), "three eq bands");
    TEST_ASSERT_EQ(eq[0].min_val, -10, "bass min");
    TEST_ASSERT_EQ(eq[0].max_val, 10, "bass max");
    TEST_ASSERT_EQ(eq[0].current, 0, "bass current");
    TEST_ASSERT_EQ(eq[1].current, -3, "mid current (signed)");
    TEST_ASSERT_EQ(eq[2].current, 5, "treble current");
    TEST_ASSERT_EQ(eq[2].band_id, 2, "treble band id");

    // [1.10] multipoint: bit 0 is the toggle (bit 1 is stuck on; see the
    // comment on parse_multipoint). Captured: 03 = on, 02 = off.
    TEST_ASSERT(bmap::parse_multipoint(V({0x03})), "multipoint 03 is on");
    TEST_ASSERT(bmap::parse_multipoint(V({0x01})), "multipoint 01 is on (bit0 set)");
    TEST_ASSERT(!bmap::parse_multipoint(V({0x02})), "multipoint 02 is off (bit0 clear)");
    TEST_ASSERT(!bmap::parse_multipoint(V({0x00})), "multipoint 00 is off");

    // [1.11] sidetone: 01 02 0f -> medium (p[1] = 2)
    TEST_ASSERT(bmap::parse_sidetone(V({0x01, 0x02, 0x0f})) == "medium", "sidetone 02 is medium");
    TEST_ASSERT(bmap::parse_sidetone(V({0x01, 0x00})) == "off", "sidetone 00 is off");
    TEST_ASSERT(bmap::parse_sidetone(V({0x01, 0x01})) == "high", "sidetone 01 is high");
    TEST_ASSERT(bmap::parse_sidetone(V({0x01, 0x03})) == "low", "sidetone 03 is low");

    // [1.3] voice prompts: 82 00 01 81 5e -> disabled, language 2 = French
    auto prompts = bmap::parse_voice_prompts(V({0x82, 0x00, 0x01, 0x81, 0x5e}));
    TEST_ASSERT(!prompts.first, "prompts disabled (bit5 clear in 0x82)");
    TEST_ASSERT(prompts.second == "French", "language 2 is French");
    auto promptsOn = bmap::parse_voice_prompts(V({0x21}));
    TEST_ASSERT(promptsOn.first && promptsOn.second == "US English", "0x21 = on, US English");

    // [2.2] battery: byte0 = percent
    TEST_ASSERT_EQ(bmap::parse_battery(V({0x58})), 88, "battery 88%");

    TEST_PASS("FieldParsers");
}

// ---------------------------------------------------------------------------
// 4. Write Payload Builders
// ---------------------------------------------------------------------------
void testBuilders() {
    TEST_CASE("Builders");

    auto cnc = bmap::build_cnc(5);
    TEST_ASSERT(cnc == V({5, 1}), "cnc payload {10 - level, 1} (midpoint is symmetric)");
    TEST_ASSERT(bmap::build_cnc(10) == V({0, 1}), "cnc max ANC -> raw 0");
    TEST_ASSERT(bmap::build_cnc(0) == V({10, 1}), "cnc passthrough -> raw 10");

    auto eqNeg = bmap::build_eq_band(-3, 1);
    TEST_ASSERT(eqNeg == V({0xfd, 0x01}), "eq payload {signed value, band id}");

    auto st = bmap::build_sidetone(2);
    TEST_ASSERT(st == V({1, 2}), "sidetone payload {1, level id}");

    // Prompts toggle preserves the language bits (0x82: lang 2, disabled)
    auto on = bmap::build_voice_prompts(true, 0x82);
    TEST_ASSERT(on == V({0x22}), "prompts on keeps lang 2");
    auto off = bmap::build_voice_prompts(false, 0x82);
    TEST_ASSERT(off == V({0x02}), "prompts off keeps lang 2");

    TEST_ASSERT(bmap::build_multipoint(true) == V({1}), "multipoint on");
    TEST_ASSERT(bmap::build_multipoint(false) == V({0}), "multipoint off");

    // Sidetone name <-> id
    TEST_ASSERT_EQ(bmap::sidetone_id_from_name("medium").value_or(99), 2, "medium id");
    TEST_ASSERT(!bmap::sidetone_id_from_name("loud").has_value(), "unknown sidetone rejected");

    TEST_PASS("Builders");
}

// ---------------------------------------------------------------------------
// 5. Mock Transport & BluetoothManager
// ---------------------------------------------------------------------------
void testMockTransportAndBluetoothManager() {
    TEST_CASE("MockTransportAndBluetoothManager");

    int peerFd = -1;
    BluetoothConfig config;
    auto mgr = BluetoothManager::createMock(config, &peerFd);
    TEST_ASSERT(mgr != nullptr, "mock manager created");
    TEST_ASSERT(peerFd >= 0, "peer fd handed out");

    std::atomic<bool> connectedFlag{false};
    BluetoothCallbacks cbs;
    cbs.onConnected = [&]() { connectedFlag = true; };
    mgr->setCallbacks(std::move(cbs));

    mgr->start();
    TEST_ASSERT(mgr->getState() == ConnectionState::CONNECTED, "mock connects immediately");
    TEST_ASSERT(connectedFlag.load(), "onConnected fired");
    TEST_ASSERT(mgr->getCurrentDevice().macAddress == "4C:87:5D:A3:D1:4F", "mock device mac");
    TEST_ASSERT_EQ(mgr->getCurrentChannel(), 8, "NC700 channel 8");

    // Packet round-trip over the socketpair
    auto pkt = bmap::bmap_packet(1, 5, bmap::Operator::Get);
    TEST_ASSERT(mgr->sendPacket(pkt), "send on mock link");
    uint8_t buf[64];
    ssize_t n = ::recv(peerFd, buf, sizeof(buf), 0);
    TEST_ASSERT_EQ(n, ssize_t(pkt.size()), "peer received packet");
    TEST_ASSERT(std::memcmp(buf, pkt.data(), pkt.size()) == 0, "bytes intact");

    mgr->disconnect();
    TEST_ASSERT(mgr->getState() == ConnectionState::DISCONNECTED, "disconnect state");

    mgr->stop();
    ::close(peerFd);

    TEST_PASS("MockTransportAndBluetoothManager");
}

// ---------------------------------------------------------------------------
// 5b. ACL gate & backoff schedule
// ---------------------------------------------------------------------------
void testAclGateAndBackoff() {
    TEST_CASE("AclGateAndBackoff");

    // Backoff: grows from 2 s, capped at 60 s, jitter within [base, base+20%]
    BluetoothConfig cfg;
    uint32_t b1 = computeBackoffMs(cfg, 1);
    uint32_t b8 = computeBackoffMs(cfg, 8);
    uint32_t b30 = computeBackoffMs(cfg, 30);
    TEST_ASSERT(b1 >= 3000 && b1 <= 3600, "attempt 1 ~3s with jitter");
    TEST_ASSERT(b8 > b1, "backoff grows");
    TEST_ASSERT(b30 >= 60000 && b30 <= 72000, "backoff capped at 60s + jitter");

    // Gate: headset paired but ACL link down -> no RFCOMM attempt, BlueZ asked
    auto transport = std::make_unique<MockTransport>();
    auto* transportPtr = transport.get();
    auto discovery = std::make_unique<MockDeviceDiscovery>();
    auto* discoveryPtr = discovery.get();
    BluetoothDeviceInfo dev;
    dev.macAddress = "4C:87:5D:A3:D1:4F";
    dev.name = "Panthère";
    dev.paired = true;
    dev.connected = false;  // headset off
    discovery->devices.push_back(dev);

    BluetoothManager mgr({}, std::move(transport), std::move(discovery),
                         std::make_unique<MockSdpResolver>());
    mgr.start();
    TEST_ASSERT(mgr.getState() == ConnectionState::RECONNECT_BACKOFF,
                "waits for the ACL link instead of hammering RFCOMM");
    TEST_ASSERT(!transportPtr->isConnected(), "no RFCOMM attempt while ACL down");
    TEST_ASSERT(discoveryPtr->lastConnectRequest == "4C:87:5D:A3:D1:4F",
                "BlueZ connect requested for the headset");
    mgr.stop();

    TEST_PASS("AclGateAndBackoff");
}

// ---------------------------------------------------------------------------
// 5c. BlueZ watcher: path matching & event-driven link tracking
// ---------------------------------------------------------------------------
void testBluezWatcherEvents() {
    TEST_CASE("BluezWatcherEvents");

    // Pure path matcher (bus-free)
    TEST_ASSERT(bluezDevicePathMatches("/org/bluez/hci0/dev_4C_87_5D_A3_D1_4F", "4C:87:5D:A3:D1:4F"),
                "hci0 device path matches");
    TEST_ASSERT(bluezDevicePathMatches("/org/bluez/hci1/dev_4C_87_5D_A3_D1_4F", "4C:87:5D:A3:D1:4F"),
                "any adapter matches");
    TEST_ASSERT(bluezDevicePathMatches("/org/bluez/hci0/dev_4C_87_5D_A3_D1_4F", "4c:87:5d:a3:d1:4f"),
                "MAC match is case-insensitive");
    TEST_ASSERT(!bluezDevicePathMatches("/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF", "4C:87:5D:A3:D1:4F"),
                "another MAC is rejected");
    TEST_ASSERT(!bluezDevicePathMatches("/org/other/dev_4C_87_5D_A3_D1_4F", "4C:87:5D:A3:D1:4F"),
                "non-BlueZ path is rejected");
    TEST_ASSERT(!bluezDevicePathMatches("/org/bluez/hci0", "4C:87:5D:A3:D1:4F"),
                "adapter path is rejected");
    TEST_ASSERT(!bluezDevicePathMatches("/org/bluez/hci0/dev_4C_87_5D_A3_D1_4F", "bogus"),
                "invalid MAC is rejected");

    // An inactive watcher degrades gracefully for the poll loop
    BluezWatcher watcher;
    TEST_ASSERT(!watcher.isActive(), "watcher is inactive before start");
    TEST_ASSERT_EQ(watcher.getPollFd(), -1, "no fd without a bus");
    TEST_ASSERT_EQ(watcher.getPollEvents(), short(0), "no poll events without a bus");

    // Event-driven manager: paired but ACL-down parks, Connected=true connects
    // immediately, Connected=false drops the link.
    auto transport = std::make_unique<MockTransport>();
    auto* transportPtr = transport.get();
    auto discovery = std::make_unique<MockDeviceDiscovery>();
    auto* discoveryPtr = discovery.get();
    BluetoothDeviceInfo dev;
    dev.macAddress = "4C:87:5D:A3:D1:4F";
    dev.name = "Panthère";
    dev.paired = true;
    dev.connected = false;  // headset off
    discovery->devices.push_back(dev);

    BluetoothManager mgr({}, std::move(transport), std::move(discovery),
                         std::make_unique<MockSdpResolver>());
    mgr.start();
    TEST_ASSERT(mgr.getState() == ConnectionState::RECONNECT_BACKOFF, "parked while ACL down");
    TEST_ASSERT(mgr.waitingForAclLink(), "parked state is observable");
    TEST_ASSERT(!transportPtr->isConnected(), "no RFCOMM attempt while parked");

    // The wake-up nudge is throttled: a connect request was just sent by
    // start(), so an immediate nudge is a no-op.
    discoveryPtr->lastConnectRequest.clear();
    mgr.requestConnectWakeUp();
    TEST_ASSERT(discoveryPtr->lastConnectRequest.empty(), "wake-up nudge is throttled");

    // Connected=true event: immediate discovery->SDP->RFCOMM, no timer wait.
    discoveryPtr->devices[0].connected = true; // BlueZ now sees the link up
    mgr.onBluezConnectedChange(true);
    TEST_ASSERT(mgr.getState() == ConnectionState::CONNECTED, "event-driven immediate connect");
    TEST_ASSERT(transportPtr->isConnected(), "RFCOMM up right after the event");
    TEST_ASSERT(!mgr.waitingForAclLink(), "no longer parked");

    // Connected=false event: drop the link like a lost RFCOMM connection.
    mgr.onBluezConnectedChange(false);
    TEST_ASSERT(mgr.getState() == ConnectionState::RECONNECT_BACKOFF, "link dropped on ACL-down event");
    TEST_ASSERT(!transportPtr->isConnected(), "transport torn down on ACL-down event");

    mgr.stop();

    TEST_PASS("BluezWatcherEvents");
}

// ---------------------------------------------------------------------------
// 6. StateEngine & status.json
// ---------------------------------------------------------------------------
void testStateEngine() {
    TEST_CASE("StateEngine");

    auto tmpDir = std::filesystem::temp_directory_path() /
                  ("bose700-test-" + std::to_string(::getpid()));

    {
        StateEngine engine(tmpDir);
        TEST_ASSERT(engine.initialize(false), "initialize");
        TEST_ASSERT(engine.getStateFilePath().filename() == "status.json", "status.json path");
        TEST_ASSERT(engine.getStateFilePath().parent_path().filename() == "bose-700",
                    "state dir is bose-700");

        // Disconnected state is minimal
        std::string json = engine.getStatusJson();
        TEST_ASSERT(json.find("\"schema\":1") != std::string::npos, "schema field");
        TEST_ASSERT(json.find("\"connected\":false") != std::string::npos, "disconnected");
        TEST_ASSERT(json.find("\"battery\"") == std::string::npos, "unknown battery omitted");
        TEST_ASSERT(json.find("\"updated_at\"") != std::string::npos, "updated_at present");

        // Populate everything and check the schema shape
        engine.setConnected(true);
        engine.setDeviceInfo("Panthère", "1.8.2-11524+e0f7590", "4C:87:5D:A3:D1:4F");
        engine.setBatteryLevel(88);
        engine.setCnc(0, 10);
        engine.setEq(0, 0, 0, -10, 10);
        engine.setSidetone("medium");
        engine.setVoicePrompts(false, "French");
        engine.setMultipoint(true);

        json = engine.getStatusJson();
        TEST_ASSERT(json.find("\"connected\":true") != std::string::npos, "connected");
        TEST_ASSERT(json.find("\"name\":\"Panthère\"") != std::string::npos, "device name (UTF-8)");
        TEST_ASSERT(json.find("\"model\":\"Bose NC Headphones 700\"") != std::string::npos, "model");
        TEST_ASSERT(json.find("\"firmware\":\"1.8.2-11524+e0f7590\"") != std::string::npos, "firmware");
        TEST_ASSERT(json.find("\"address\":\"4C:87:5D:A3:D1:4F\"") != std::string::npos, "address");
        TEST_ASSERT(json.find("\"battery\":{\"level\":88}") != std::string::npos, "battery");
        TEST_ASSERT(json.find("\"cnc\":{\"level\":0,\"max\":10}") != std::string::npos, "cnc");
        TEST_ASSERT(json.find("\"eq\":{\"bass\":0,\"mid\":0,\"treble\":0,\"min\":-10,\"max\":10}") != std::string::npos, "eq");
        TEST_ASSERT(json.find("\"sidetone\":\"medium\"") != std::string::npos, "sidetone");
        TEST_ASSERT(json.find("\"voice_prompts\":{\"enabled\":false,\"language\":\"French\"}") != std::string::npos, "prompts");
        TEST_ASSERT(json.find("\"multipoint\":true") != std::string::npos, "multipoint");

        // File exists on disk with mode 0600
        struct stat st{};
        TEST_ASSERT(::stat(engine.getStateFilePath().c_str(), &st) == 0, "status.json exists");
        TEST_ASSERT_EQ(st.st_mode & 0777, 0600, "status.json mode 0600");

        // No leftover temp files after a save
        bool tmpLeft = false;
        for (const auto& entry : std::filesystem::directory_iterator(engine.getStateFilePath().parent_path())) {
            if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
                tmpLeft = true;
            }
        }
        TEST_ASSERT(!tmpLeft, "no leftover tmp files");

        engine.cleanup();
        TEST_ASSERT(!std::filesystem::exists(engine.getStateFilePath()), "cleanup removes status.json");
    }

    std::filesystem::remove_all(tmpDir);

    TEST_PASS("StateEngine");
}

// ---------------------------------------------------------------------------
// 7. IpcServer command surface (JSON contract)
// ---------------------------------------------------------------------------
void testIpcServer() {
    TEST_CASE("IpcServer");

    IpcServer server;

    IpcCallbacks cbs;
    cbs.getStatusJson = []() { return std::string("{\"schema\":1,\"connected\":false}"); };
    int lastCnc = -1;
    cbs.setCnc = [&](int level, std::string&) { lastCnc = level; return true; };
    cbs.setEq = [](int, int, int, std::string&) { return true; };
    cbs.setSidetone = [](const std::string&, std::string&) { return true; };
    cbs.setPrompts = [](bool, std::string&) { return true; };
    cbs.setMultipoint = [](bool, std::string&) { return true; };
    cbs.reconnect = [](std::string&) { return true; };
    server.setCallbacks(std::move(cbs));

    auto isOk = [](const std::string& r) { return r.rfind("{\"ok\":true", 0) == 0; };
    auto isErr = [](const std::string& r) { return r.rfind("{\"ok\":false,\"error\":", 0) == 0; };

    // status returns the full state JSON with ok merged in
    auto status = server.handleCommandLine("status");
    TEST_ASSERT(isOk(status), "status ok");
    TEST_ASSERT(status.find("\"schema\":1") != std::string::npos, "status carries state");
    TEST_ASSERT(status.find("\"connected\":false") != std::string::npos, "status connected field");

    // cnc
    TEST_ASSERT(isOk(server.handleCommandLine("cnc 5")), "cnc 5");
    TEST_ASSERT_EQ(lastCnc, 5, "cnc level passed through");
    TEST_ASSERT(isOk(server.handleCommandLine("cnc 0")), "cnc 0");
    TEST_ASSERT(isOk(server.handleCommandLine("cnc 10")), "cnc 10");
    TEST_ASSERT(isErr(server.handleCommandLine("cnc 11")), "cnc 11 rejected");
    TEST_ASSERT(isErr(server.handleCommandLine("cnc -1")), "cnc -1 rejected");
    TEST_ASSERT(isErr(server.handleCommandLine("cnc")), "cnc without level rejected");
    TEST_ASSERT(isErr(server.handleCommandLine("cnc loud")), "cnc non-numeric rejected");

    // eq
    TEST_ASSERT(isOk(server.handleCommandLine("eq 0 0 0")), "eq zeros");
    TEST_ASSERT(isOk(server.handleCommandLine("eq -10 5 10")), "eq extremes");
    TEST_ASSERT(isErr(server.handleCommandLine("eq 0 0")), "eq needs 3 values");
    TEST_ASSERT(isErr(server.handleCommandLine("eq 0 0 11")), "eq range checked");
    TEST_ASSERT(isErr(server.handleCommandLine("eq 0 0 -11")), "eq negative range checked");
    TEST_ASSERT(isErr(server.handleCommandLine("eq a b c")), "eq non-numeric rejected");

    // sidetone
    TEST_ASSERT(isOk(server.handleCommandLine("sidetone off")), "sidetone off");
    TEST_ASSERT(isOk(server.handleCommandLine("sidetone low")), "sidetone low");
    TEST_ASSERT(isOk(server.handleCommandLine("sidetone medium")), "sidetone medium");
    TEST_ASSERT(isOk(server.handleCommandLine("sidetone high")), "sidetone high");
    TEST_ASSERT(isErr(server.handleCommandLine("sidetone loud")), "sidetone rejects nonsense");

    // prompts / multipoint
    TEST_ASSERT(isOk(server.handleCommandLine("prompts on")), "prompts on");
    TEST_ASSERT(isOk(server.handleCommandLine("prompts off")), "prompts off");
    TEST_ASSERT(isErr(server.handleCommandLine("prompts maybe")), "prompts rejects nonsense");
    TEST_ASSERT(isOk(server.handleCommandLine("multipoint on")), "multipoint on");
    TEST_ASSERT(isOk(server.handleCommandLine("multipoint off")), "multipoint off");
    TEST_ASSERT(isErr(server.handleCommandLine("multipoint")), "multipoint needs a value");

    // reconnect
    TEST_ASSERT(isOk(server.handleCommandLine("reconnect")), "reconnect");

    // unknown / empty
    TEST_ASSERT(isErr(server.handleCommandLine("frobnicate")), "unknown command");
    TEST_ASSERT(isErr(server.handleCommandLine("")), "empty command");

    // Daemon-side failure propagates as an error JSON
    IpcCallbacks failing;
    failing.setCnc = [](int, std::string& err) { err = "headset not connected"; return false; };
    server.setCallbacks(std::move(failing));
    auto failResp = server.handleCommandLine("cnc 3");
    TEST_ASSERT(isErr(failResp), "daemon error surfaces");
    TEST_ASSERT(failResp.find("headset not connected") != std::string::npos, "error text carried");

    TEST_PASS("IpcServer");
}

// ---------------------------------------------------------------------------
// 8. IpcServer over a real UNIX socket
// ---------------------------------------------------------------------------
void testIpcServerSocket() {
    TEST_CASE("IpcServerSocket");

    auto tmpDir = std::filesystem::temp_directory_path() /
                  ("bose700-ipc-test-" + std::to_string(::getpid()));
    std::filesystem::create_directories(tmpDir);

    IpcServer server(tmpDir.string());
    TEST_ASSERT(server.start(), "server starts");
    TEST_ASSERT(server.getSocketPath().find("bose-700.sock") != std::string::npos, "socket name");

    struct stat st{};
    TEST_ASSERT(::stat(server.getSocketPath().c_str(), &st) == 0, "socket exists");
    TEST_ASSERT_EQ(st.st_mode & 0777, 0700, "socket mode 0700");

    IpcCallbacks cbs;
    cbs.getStatusJson = []() { return std::string("{\"schema\":1,\"connected\":true}"); };
    server.setCallbacks(std::move(cbs));

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    TEST_ASSERT(fd >= 0, "client socket");

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, server.getSocketPath().c_str(), sizeof(addr.sun_path) - 1);
    TEST_ASSERT(::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0,
                "client connects");

    const char* cmd = "status\n";
    TEST_ASSERT(::send(fd, cmd, std::strlen(cmd), 0) == ssize_t(std::strlen(cmd)), "command sent");

    // Pump the server until the response arrives
    std::string response;
    for (int i = 0; i < 200 && response.find('\n') == std::string::npos; ++i) {
        std::vector<pollfd> pfds;
        server.appendPollFds(pfds);
        ::poll(pfds.data(), pfds.size(), 5);
        server.handlePollEvents(pfds);

        pollfd cfd{fd, POLLIN, 0};
        if (::poll(&cfd, 1, 5) > 0 && (cfd.revents & POLLIN)) {
            char buf[1024];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n > 0) response.append(buf, static_cast<size_t>(n));
        }
    }

    ::close(fd);
    server.stop();
    std::filesystem::remove_all(tmpDir);

    TEST_ASSERT(response.rfind("{\"ok\":true,", 0) == 0, "socket response is ok JSON");
    TEST_ASSERT(response.find("\"connected\":true") != std::string::npos, "socket response carries state");

    TEST_PASS("IpcServerSocket");
}

// ---------------------------------------------------------------------------
// 9. IpcServer subscribe/broadcast
//
// The bar widget subscribes and is then pushed every state change, so it never
// reads the state file. A subscriber that stops reading must be dropped rather
// than allowed to grow the daemon's memory.
// ---------------------------------------------------------------------------
void testIpcSubscribeAndBroadcast() {
    TEST_CASE("IpcServer subscribe/broadcast");

    char tmpl[] = "/tmp/test_bose700_sub_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp for subscribe test must succeed");
    const std::string sockPath = std::string(sandbox) + "/test.sock";

    std::string statusJson = R"({"schema":1,"connected":true,"battery":{"level":42}})";

    IpcServer server(sockPath);
    IpcCallbacks cb;
    cb.getStatusJson = [&statusJson]() { return statusJson; };
    server.setCallbacks(cb);
    TEST_ASSERT(server.start(), "IpcServer start must succeed");

    auto connectClient = [&sockPath]() {
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
        if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd);
            return -1;
        }
        return fd;
    };
    auto readLine = [](int fd, int tries = 20) {
        std::string out;
        char c = 0;
        for (int i = 0; i < tries * 1000 && out.find('\n') == std::string::npos; ++i) {
            ssize_t n = ::recv(fd, &c, 1, MSG_DONTWAIT);
            if (n == 1) out.push_back(c);
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            else if (n <= 0) break;
        }
        return out;
    };

    const int subscriber = connectClient();
    const int bystander = connectClient();
    TEST_ASSERT(subscriber >= 0 && bystander >= 0, "both clients must connect");
    server.pollOnce(50);
    TEST_ASSERT_EQ(server.getClientCount(), 2UL, "server must hold two clients");
    TEST_ASSERT_EQ(server.getSubscriberCount(), 0UL, "nobody is subscribed yet");

    ::send(subscriber, "subscribe\n", 10, 0);
    server.pollOnce(50);
    TEST_ASSERT_EQ(server.getSubscriberCount(), 1UL, "subscribe must register the session");
    TEST_ASSERT_EQ(readLine(subscriber), statusJson + "\n", "subscribe answers with the current status");

    statusJson = R"({"schema":1,"connected":true,"battery":{"level":41}})";
    server.broadcastStatus(statusJson);
    TEST_ASSERT_EQ(readLine(subscriber), statusJson + "\n", "a state change is pushed to the subscriber");
    TEST_ASSERT_EQ(readLine(bystander), std::string(), "a client that never subscribed is not pushed to");

    // A status is only sent whole: broadcastStatus appends the newline itself.
    server.broadcastStatus(statusJson + "\n");
    TEST_ASSERT_EQ(readLine(subscriber), statusJson + "\n", "an already terminated status is not doubled");

    ::send(subscriber, "unsubscribe\n", 12, 0);
    server.pollOnce(50);
    TEST_ASSERT_EQ(readLine(subscriber), std::string("{\"ok\":true}\n"), "unsubscribe is acknowledged");
    TEST_ASSERT_EQ(server.getSubscriberCount(), 0UL, "unsubscribe clears the session");
    server.broadcastStatus(statusJson);
    TEST_ASSERT_EQ(readLine(subscriber), std::string(), "nothing is pushed after unsubscribe");

    // A subscriber that stops reading: the queue must be capped and the client
    // dropped, not grown without bound.
    ::send(subscriber, "subscribe\n", 10, 0);
    server.pollOnce(50);
    TEST_ASSERT_EQ(server.getSubscriberCount(), 1UL, "resubscribe works");
    const std::string bulky = R"({"schema":1,"padding":")" + std::string(8192, 'x') + R"("})";
    for (int i = 0; i < 200 && server.getSubscriberCount() > 0; ++i) {
        server.broadcastStatus(bulky);
    }
    TEST_ASSERT_EQ(server.getSubscriberCount(), 0UL, "a subscriber that stops reading is dropped");
    TEST_ASSERT_EQ(server.getClientCount(), 1UL, "only the stalled client is dropped");

    ::close(subscriber);
    ::close(bystander);
    server.stop();
    ::unlink(sockPath.c_str());
    ::rmdir(sandbox);

    TEST_PASS("IpcServer subscribe/broadcast");
}

// ---------------------------------------------------------------------------
// 10. Socket activation: the service manager holds the listening socket for
// the session, so the daemon must adopt the descriptor it is handed instead of
// binding the path itself.
// ---------------------------------------------------------------------------
void testIpcSocketActivation() {
    TEST_CASE("IpcServer socket activation");

    char tmpl[] = "/tmp/test_bose700_act_XXXXXX";
    char* sandbox = ::mkdtemp(tmpl);
    TEST_ASSERT(sandbox != nullptr, "mkdtemp for activation test must succeed");
    const std::string sockPath = std::string(sandbox) + "/activated.sock";

    // Stand in for systemd: bind and listen here, then hand fd 3 over.
    int bound = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    TEST_ASSERT(bound >= 0, "listener creation must succeed");
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sockPath.c_str(), sizeof(addr.sun_path) - 1);
    TEST_ASSERT_EQ(::bind(bound, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0, "bind must succeed");
    TEST_ASSERT_EQ(::listen(bound, 8), 0, "listen must succeed");

    constexpr int kListenFdsStart = 3;
    if (bound != kListenFdsStart) {
        TEST_ASSERT(::dup2(bound, kListenFdsStart) == kListenFdsStart, "handover to fd 3 must succeed");
        ::close(bound);
    }
    ::setenv("LISTEN_PID", std::to_string(::getpid()).c_str(), 1);
    ::setenv("LISTEN_FDS", "1", 1);

    {
        IpcServer server;
        TEST_ASSERT(server.start(), "start must succeed with an inherited socket");
        TEST_ASSERT(server.isSocketActivated(), "the inherited descriptor must be adopted");
        TEST_ASSERT_EQ(server.getSocketPath(), sockPath, "the adopted socket keeps its bound path");
        TEST_ASSERT_EQ(server.getListenFd(), kListenFdsStart, "the passed descriptor is used as-is");
        TEST_ASSERT(::getenv("LISTEN_FDS") == nullptr, "LISTEN_FDS is consumed, never inherited twice");

        // A client reaches the adopted listener.
        int clientFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        TEST_ASSERT_EQ(::connect(clientFd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)), 0,
                       "connect to the adopted socket must succeed");
        server.pollOnce(50);
        TEST_ASSERT_EQ(server.getClientCount(), 1UL, "the adopted listener accepts clients");
        ::close(clientFd);
        server.stop();

        // Stopping must leave the manager's socket bound: unlinking it would
        // open exactly the window socket activation exists to close.
        struct stat st{};
        TEST_ASSERT_EQ(::stat(sockPath.c_str(), &st), 0, "an adopted socket is left in place on stop");
    }

    // Without the environment, the daemon binds for itself as before.
    {
        const std::string ownPath = std::string(sandbox) + "/own.sock";
        IpcServer server(ownPath);
        TEST_ASSERT(server.start(), "start must succeed without socket activation");
        TEST_ASSERT(!server.isSocketActivated(), "a self-bound socket is not reported as activated");
        server.stop();

        struct stat st{};
        TEST_ASSERT(::stat(ownPath.c_str(), &st) != 0, "a self-bound socket is cleaned up on stop");
    }

    ::unlink(sockPath.c_str());
    ::rmdir(sandbox);
    TEST_PASS("IpcServer socket activation");
}

// ---------------------------------------------------------------------------
// Main Runner
// ---------------------------------------------------------------------------
int main() {
    std::cout << "========================================\n";
    std::cout << "  omarchy-bose-700: Protocol & Mock Tests\n";
    std::cout << "========================================\n";

    testPacketRoundTrip();
    testStreamParser();
    testFieldParsers();
    testBuilders();
    testMockTransportAndBluetoothManager();
    testAclGateAndBackoff();
    testBluezWatcherEvents();
    testStateEngine();
    testIpcServer();
    testIpcServerSocket();
    testIpcSubscribeAndBroadcast();
    testIpcSocketActivation();

    std::cout << "========================================\n";
    std::cout << "Summary: " << (gTotalTests - gFailedTests) << "/" << gTotalTests
              << " test cases passed (" << gFailedTests << " failed)\n";
    std::cout << "========================================\n";

    return (gFailedTests == 0) ? 0 : 1;
}
