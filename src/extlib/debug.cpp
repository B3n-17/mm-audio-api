#include <extlib/debug.hpp>
#include <audio_debug_html.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <plog/Log.h>

#if defined(_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
#endif

namespace {

constexpr size_t EVENT_CAPACITY = 4096;

using Clock = std::chrono::system_clock;

uint64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
}

struct EventStore {
    std::array<Debug::Event, EVENT_CAPACITY> events;
    size_t head = 0;
    size_t size = 0;
    uint64_t nextId = 1;
    uint64_t dropped = 0;
    std::mutex mutex;
};

EventStore sEventStore;
Debug::Snapshot sSnapshot = { 0 };
std::mutex sSnapshotMutex;

std::atomic<bool> sEnabled = false;
std::once_flag sHttpStartOnce;

std::unordered_map<uint32_t, std::string> sTagRegistry;
std::mutex sTagRegistryMutex;

// Streamed audio state: keyed by (resourceId << 8 | trackNo)
std::unordered_map<uint64_t, Debug::StreamState> sStreamStates;
std::mutex sStreamMutex;

// DJ mixer overrides: indexed by [playerIndex][channelIndex]
constexpr size_t MAX_MIX_CHANNELS = 16;
std::array<std::array<Debug::MixOverride, MAX_MIX_CHANNELS>, Debug::MAX_PLAYERS> sMixOverrides;
std::mutex sMixMutex;
std::atomic<bool> sMixerOpen = false;

#if defined(_WIN32)
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
inline void closeSocket(SocketHandle s) { closesocket(s); }
#else
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
inline void closeSocket(SocketHandle s) { close(s); }
#endif

uint64_t parseU64(const std::string& value, uint64_t fallback) {
    if (value.empty()) {
        return fallback;
    }

    try {
        return std::stoull(value);
    } catch (...) {
        return fallback;
    }
}

std::string queryValue(const std::string& query, const std::string& key) {
    size_t pos = 0;
    while (pos < query.size()) {
        size_t eq = query.find('=', pos);
        if (eq == std::string::npos) {
            break;
        }
        size_t amp = query.find('&', eq + 1);
        std::string k = query.substr(pos, eq - pos);
        std::string v = query.substr(eq + 1, amp == std::string::npos ? std::string::npos : amp - (eq + 1));
        if (k == key) {
            return v;
        }
        if (amp == std::string::npos) {
            break;
        }
        pos = amp + 1;
    }
    return {};
}

void sendHttp(SocketHandle client, const char* status, const char* contentType, const std::string& body) {
    std::ostringstream out;
    out << "HTTP/1.1 " << status << "\r\n"
        << "Content-Type: " << contentType << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n"
        << "Content-Length: " << body.size() << "\r\n\r\n"
        << body;

    std::string payload = out.str();
    send(client, payload.data(), static_cast<int>(payload.size()), 0);
}

std::string snapshotJson() {
    auto snapshot = Debug::getSnapshot();

    auto roleName = [](size_t i) -> const char* {
        switch (i) {
            case 0: return "BGM_MAIN";
            case 1: return "FANFARE";
            case 2: return "SFX";
            case 3: return "BGM_SUB";
            case 4: return "AMBIENCE";
            default: return "UNUSED";
        }
    };

    std::ostringstream out;
    out << "{\"ts_ms\":" << snapshot.tsMs
        << ",\"init_phase\":" << snapshot.initPhase
        << ",\"khz_mode\":" << (snapshot.khzMode ? "true" : "false")
        << ",\"main_seq_id\":" << snapshot.mainSeqId
        << ",\"sub_seq_id\":" << snapshot.subSeqId
        << ",\"fanfare_seq_id\":" << snapshot.fanfareSeqId
        << ",\"ambience_seq_id\":" << snapshot.ambienceSeqId
        << ",\"players\":[";

    for (size_t i = 0; i < snapshot.players.size(); i++) {
        if (i != 0) {
            out << ',';
        }

        const auto& p = snapshot.players[i];
        out << "{\"player\":" << i
            << ",\"role\":\"" << roleName(i) << "\""
            << ",\"seq_id\":" << p.seqId
            << ",\"enabled\":" << p.enabled
            << ",\"waiting_for_fonts\":" << p.waitingForFonts
            << ",\"setup_cmd_num\":" << p.setupCmdNum
            << ",\"active_channel_count\":" << p.activeChannelCount
            << ",\"active_channel_mask\":" << p.activeChannelMask
            << ",\"notes_active\":" << p.notesActive
            << ",\"notes_decaying\":" << p.notesDecaying
            << ",\"notes_releasing\":" << p.notesReleasing
            << ",\"pan_average\":" << p.panAverage
            << ",\"pan_left_count\":" << p.panLeftCount
            << ",\"pan_center_count\":" << p.panCenterCount
            << ",\"pan_right_count\":" << p.panRightCount
            << ",\"seq_io\":[";

        for (size_t io = 0; io < p.seqIo.size(); io++) {
            if (io != 0) {
                out << ',';
            }
            out << p.seqIo[io];
        }

        out << "]"
            << ",\"ch_enabled\":[";

        for (size_t ch = 0; ch < p.chEnabled.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chEnabled[ch];
        }

        out << "]"
            << ",\"ch_pan\":[";

        for (size_t ch = 0; ch < p.chPan.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chPan[ch];
        }

        out << "]"
            << ",\"ch_vol_milli\":[";

        for (size_t ch = 0; ch < p.chVolMilli.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chVolMilli[ch];
        }

        out << "]"
            << ",\"ch_reverb_vol\":[";

        for (size_t ch = 0; ch < p.chReverbVol.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.chReverbVol[ch];
        }

        out << "]"
            << ",\"layer_enabled_mask\":[";

        for (size_t ch = 0; ch < p.layerEnabledMask.size(); ch++) {
            if (ch != 0) {
                out << ',';
            }
            out << p.layerEnabledMask[ch];
        }

        out << "]"
            << ",\"layer_pan\":[";

        for (size_t idx = 0; idx < p.layerPan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerPan[idx];
        }

        out << "]"
            << ",\"layer_note_pan\":[";

        for (size_t idx = 0; idx < p.layerNotePan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNotePan[idx];
        }

        out << "]"
            << ",\"layer_note_attr_pan\":[";

        for (size_t idx = 0; idx < p.layerNoteAttrPan.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteAttrPan[idx];
        }

        out << "]"
            << ",\"layer_note_target_l\":[";

        for (size_t idx = 0; idx < p.layerNoteTargetL.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteTargetL[idx];
        }

        out << "]"
            << ",\"layer_note_target_r\":[";

        for (size_t idx = 0; idx < p.layerNoteTargetR.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteTargetR[idx];
        }

        out << "]"
            << ",\"layer_note_cur_l\":[";

        for (size_t idx = 0; idx < p.layerNoteCurL.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteCurL[idx];
        }

        out << "]"
            << ",\"layer_note_cur_r\":[";

        for (size_t idx = 0; idx < p.layerNoteCurR.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerNoteCurR[idx];
        }

        out << "]"
            << ",\"layer_sample_medium_codec\":[";

        for (size_t idx = 0; idx < p.layerSampleMediumCodec.size(); idx++) {
            if (idx != 0) {
                out << ',';
            }
            out << p.layerSampleMediumCodec[idx];
        }

        out << "]}";
    }

    out << "]}";
    return out.str();
}

std::string eventsJson(uint64_t sinceId, size_t limit) {
    auto events = Debug::getEventsSince(sinceId, limit);
    uint64_t lastId = Debug::lastEventId();
    uint64_t dropped = Debug::droppedEventCount();

    std::ostringstream out;
    out << "{\"since\":" << sinceId
        << ",\"last_event_id\":" << lastId
        << ",\"dropped_total\":" << dropped
        << ",\"events\":[";

    for (size_t i = 0; i < events.size(); i++) {
        if (i != 0) {
            out << ',';
        }
        const auto& e = events[i];
        out << "{\"id\":" << e.id
            << ",\"ts_ms\":" << e.tsMs
            << ",\"tag\":" << e.tag
            << ",\"a\":" << e.a
            << ",\"b\":" << e.b
            << ",\"c\":" << e.c
            << ",\"d\":" << e.d << '}';
    }

    out << "]}";
    return out.str();
}

// Parse a JSON integer field: { ..., "key": value, ... }
// Returns defaultVal if the key is not found or the value is not a plain integer.
static int32_t jsonInt(const std::string& body, const std::string& key, int32_t defaultVal) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = body.find(needle);
    if (pos == std::string::npos) {
        return defaultVal;
    }
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':')) {
        pos++;
    }
    if (pos >= body.size()) {
        return defaultVal;
    }
    // Accept optional leading '-'
    size_t start = pos;
    if (body[start] == '-') {
        start++;
    }
    if (start >= body.size() || !std::isdigit(static_cast<unsigned char>(body[start]))) {
        return defaultVal;
    }
    try {
        size_t eaten = 0;
        int32_t v = static_cast<int32_t>(std::stol(body.substr(pos), &eaten));
        (void)eaten;
        return v;
    } catch (...) {
        return defaultVal;
    }
}

std::string streamsJson() {
    std::lock_guard<std::mutex> lock(sStreamMutex);
    std::ostringstream out;
    out << "{\"streams\":[";
    bool first = true;
    for (const auto& [key, s] : sStreamStates) {
        if (!first) out << ',';
        first = false;
        out << "{\"resource_id\":" << s.resourceId
            << ",\"track_no\":" << s.trackNo
            << ",\"pos\":" << s.pos
            << ",\"sample_count\":" << s.sampleCount
            << ",\"loop_start\":" << s.loopStart
            << ",\"loop_end\":" << s.loopEnd
            << ",\"loop_count\":" << s.loopCount
            << ",\"ts_ms\":" << s.tsMs
            << "}";
    }
    out << "]}";
    return out.str();
}

std::string mixOverridesJson() {
    std::lock_guard<std::mutex> lock(sMixMutex);
    std::ostringstream out;
    out << "{\"overrides\":[";
    bool first = true;
    for (size_t p = 0; p < Debug::MAX_PLAYERS; p++) {
        for (size_t c = 0; c < MAX_MIX_CHANNELS; c++) {
            const auto& ov = sMixOverrides[p][c];
            if (!ov.active) {
                continue;
            }
            if (!first) {
                out << ',';
            }
            first = false;
            out << "{\"player\":" << p
                << ",\"channel\":" << c
                << ",\"pan\":" << ov.pan
                << ",\"vol_milli\":" << ov.volMilli
                << ",\"reverb\":" << ov.reverb
                << ",\"muted\":" << (ov.muted ? "true" : "false")
                << "}";
        }
    }
    out << "]}";
    return out.str();
}

void handleClient(SocketHandle client) {
    char reqBuf[8192];
    int received = recv(client, reqBuf, sizeof(reqBuf) - 1, 0);
    if (received <= 0) {
        return;
    }

    reqBuf[received] = '\0';
    std::string request(reqBuf);
    size_t lineEnd = request.find("\r\n");
    if (lineEnd == std::string::npos) {
        sendHttp(client, "400 Bad Request", "text/plain; charset=utf-8", "Bad request");
        return;
    }

    std::string line = request.substr(0, lineEnd);

    // Handle POST /mixer-open (must be checked before POST /mix due to prefix overlap)
    if (line.rfind("POST /mixer-open", 0) == 0) {
        size_t bodyStart = request.find("\r\n\r\n");
        std::string body = bodyStart != std::string::npos ? request.substr(bodyStart + 4) : "";
        int32_t open = jsonInt(body, "open", 0);
        Debug::setMixerOpen(open != 0);
        sendHttp(client, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return;
    }

    // Handle POST /mix
    if (line.rfind("POST /mix", 0) == 0) {
        // Find body after \r\n\r\n
        size_t bodyStart = request.find("\r\n\r\n");
        std::string body = bodyStart != std::string::npos ? request.substr(bodyStart + 4) : "";

        int32_t player  = jsonInt(body, "player",   -1);
        int32_t channel = jsonInt(body, "channel",  -1);
        int32_t pan     = jsonInt(body, "pan",      -1);
        int32_t vol     = jsonInt(body, "vol_milli", -1);
        int32_t reverb  = jsonInt(body, "reverb",   -1);
        int32_t muted   = jsonInt(body, "muted",     0);
        int32_t clear   = jsonInt(body, "clear",     0);

        if (player < 0 || player >= static_cast<int32_t>(Debug::MAX_PLAYERS) ||
            channel < 0 || channel >= static_cast<int32_t>(MAX_MIX_CHANNELS)) {
            sendHttp(client, "400 Bad Request", "application/json; charset=utf-8",
                     "{\"error\":\"player/channel out of range\"}");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(sMixMutex);
            auto& ov = sMixOverrides[static_cast<size_t>(player)][static_cast<size_t>(channel)];
            if (clear) {
                ov = Debug::MixOverride{};
            } else {
                ov.active   = true;
                ov.pan      = pan;
                ov.volMilli = vol;
                ov.reverb   = reverb;
                ov.muted    = muted != 0;
            }
        }

        sendHttp(client, "200 OK", "application/json; charset=utf-8", "{\"ok\":true}");
        return;
    }

    // Handle OPTIONS preflight (CORS)
    if (line.rfind("OPTIONS ", 0) == 0) {
        std::string preflightBody;
        std::ostringstream out;
        out << "HTTP/1.1 204 No Content\r\n"
            << "Access-Control-Allow-Origin: *\r\n"
            << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            << "Access-Control-Allow-Headers: Content-Type\r\n"
            << "Connection: close\r\n\r\n";
        std::string payload = out.str();
        send(client, payload.data(), static_cast<int>(payload.size()), 0);
        return;
    }

    if (line.rfind("GET ", 0) != 0) {
        sendHttp(client, "405 Method Not Allowed", "text/plain; charset=utf-8", "Only GET and POST /mix supported");
        return;
    }

    size_t pathStart = 4;
    size_t pathEnd = line.find(' ', pathStart);
    if (pathEnd == std::string::npos) {
        sendHttp(client, "400 Bad Request", "text/plain; charset=utf-8", "Malformed request");
        return;
    }

    std::string rawPath = line.substr(pathStart, pathEnd - pathStart);
    std::string path = rawPath;
    std::string query;
    size_t qPos = rawPath.find('?');
    if (qPos != std::string::npos) {
        path = rawPath.substr(0, qPos);
        query = rawPath.substr(qPos + 1);
    }

    if (path == "/health") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8",
                 "{\"ok\":true,\"enabled\":" + std::to_string(Debug::isEnabled() ? 1 : 0) +
                     ",\"port\":" + std::to_string(Debug::HTTP_PORT) + "}");
        return;
    }

    if (path == "/snapshot") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", snapshotJson());
        return;
    }

    if (path == "/events") {
        uint64_t sinceId = parseU64(queryValue(query, "since"), 0);
        uint64_t limit = parseU64(queryValue(query, "limit"), 256);
        if (limit == 0) {
            limit = 1;
        }
        if (limit > 2048) {
            limit = 2048;
        }

        sendHttp(client, "200 OK", "application/json; charset=utf-8", eventsJson(sinceId, static_cast<size_t>(limit)));
        return;
    }

    if (path == "/tags") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", Debug::getTagsJson());
        return;
    }

    if (path == "/mix") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", mixOverridesJson());
        return;
    }

    if (path == "/streams") {
        sendHttp(client, "200 OK", "application/json; charset=utf-8", streamsJson());
        return;
    }

    if (path == "/" || path == "/audio-debug.html") {
        sendHttp(client, "200 OK", "text/html; charset=utf-8", Debug::buildAudioDebugHtml());
        return;
    }

    sendHttp(client, "404 Not Found", "text/plain; charset=utf-8", "Not found");
}

void runHttpServer() {
#if defined(_WIN32)
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        PLOG_ERROR << "Debug HTTP failed to init WinSock";
        return;
    }
#endif

    SocketHandle server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET_HANDLE) {
        PLOG_ERROR << "Debug HTTP failed to create socket";
        return;
    }

    int opt = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(Debug::HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(server, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        PLOG_ERROR << "Debug HTTP failed to bind 127.0.0.1:" << Debug::HTTP_PORT;
        closeSocket(server);
        return;
    }

    if (listen(server, 8) < 0) {
        PLOG_ERROR << "Debug HTTP failed to listen";
        closeSocket(server);
        return;
    }

    PLOG_INFO << "Audio debug HTTP running at http://127.0.0.1:" << Debug::HTTP_PORT;

    while (true) {
        SocketHandle client = accept(server, nullptr, nullptr);
        if (client == INVALID_SOCKET_HANDLE) {
            continue;
        }

        handleClient(client);
        closeSocket(client);
    }
}

} // namespace

namespace Debug {

void setEnabled(bool enabled) {
    sEnabled.store(enabled);
    if (!enabled) {
        return;
    }

    std::call_once(sHttpStartOnce, [] {
        std::thread serverThread(runHttpServer);
        serverThread.detach();
    });
}

bool isEnabled() {
    return sEnabled.load();
}

void pushEvent(uint32_t tag, int32_t a, int32_t b, int32_t c, int32_t d) {
    if (!isEnabled()) {
        return;
    }

    if (!sEventStore.mutex.try_lock()) {
        sEventStore.dropped++;
        return;
    }

    EventStore& store = sEventStore;
    const size_t idx = (store.head + store.size) % EVENT_CAPACITY;

    store.events[idx] = Event {
        .id = store.nextId++,
        .tsMs = nowMs(),
        .tag = tag,
        .a = a,
        .b = b,
        .c = c,
        .d = d,
    };

    if (store.size == EVENT_CAPACITY) {
        store.head = (store.head + 1) % EVENT_CAPACITY;
        store.dropped++;
    } else {
        store.size++;
    }

    sEventStore.mutex.unlock();
}

void setKhzMode(bool enabled) {
    std::lock_guard<std::mutex> lock(sSnapshotMutex);
    sSnapshot.khzMode = enabled;
}

void setSnapshot(uint32_t initPhase, int32_t mainSeqId, int32_t subSeqId, int32_t fanfareSeqId, int32_t ambienceSeqId) {
    if (!isEnabled()) {
        return;
    }

    if (!sSnapshotMutex.try_lock()) {
        return;
    }

    sSnapshot.tsMs = nowMs();
    sSnapshot.initPhase = initPhase;
    sSnapshot.mainSeqId = mainSeqId;
    sSnapshot.subSeqId = subSeqId;
    sSnapshot.fanfareSeqId = fanfareSeqId;
    sSnapshot.ambienceSeqId = ambienceSeqId;

    sSnapshotMutex.unlock();
}

void setSeqPlayerPacked(uint32_t playerIndex, int32_t seqId, const int32_t* packed) {
    if (!isEnabled() || playerIndex >= sSnapshot.players.size()) {
        return;
    }

    if (!sSnapshotMutex.try_lock()) {
        return;
    }

    sSnapshot.players[playerIndex] = PlayerState {
        .seqId = seqId,
        .enabled = static_cast<uint32_t>(packed[0]),
        .waitingForFonts = static_cast<uint32_t>(packed[1]),
        .setupCmdNum = static_cast<uint32_t>(packed[2]),
        .activeChannelCount = static_cast<uint32_t>(packed[3]),
        .activeChannelMask = static_cast<uint32_t>(packed[4]),
        .notesActive = packed[5],
        .notesDecaying = packed[6],
        .notesReleasing = packed[7],
        .panAverage = packed[8],
        .panLeftCount = static_cast<uint32_t>(packed[9]),
        .panCenterCount = static_cast<uint32_t>(packed[10]),
        .panRightCount = static_cast<uint32_t>(packed[11]),
        .seqIo = {
            packed[12], packed[13], packed[14], packed[15],
            packed[16], packed[17], packed[18], packed[19],
        },
        .chEnabled = {
            static_cast<uint32_t>(packed[20]), static_cast<uint32_t>(packed[21]),
            static_cast<uint32_t>(packed[22]), static_cast<uint32_t>(packed[23]),
            static_cast<uint32_t>(packed[24]), static_cast<uint32_t>(packed[25]),
            static_cast<uint32_t>(packed[26]), static_cast<uint32_t>(packed[27]),
            static_cast<uint32_t>(packed[28]), static_cast<uint32_t>(packed[29]),
            static_cast<uint32_t>(packed[30]), static_cast<uint32_t>(packed[31]),
            static_cast<uint32_t>(packed[32]), static_cast<uint32_t>(packed[33]),
            static_cast<uint32_t>(packed[34]), static_cast<uint32_t>(packed[35]),
        },
        .chPan = {
            packed[36], packed[37], packed[38], packed[39],
            packed[40], packed[41], packed[42], packed[43],
            packed[44], packed[45], packed[46], packed[47],
            packed[48], packed[49], packed[50], packed[51],
        },
        .chVolMilli = {
            packed[52], packed[53], packed[54], packed[55],
            packed[56], packed[57], packed[58], packed[59],
            packed[60], packed[61], packed[62], packed[63],
            packed[64], packed[65], packed[66], packed[67],
        },
        .chReverbVol = {
            packed[68], packed[69], packed[70], packed[71],
            packed[72], packed[73], packed[74], packed[75],
            packed[76], packed[77], packed[78], packed[79],
            packed[80], packed[81], packed[82], packed[83],
        },
        .layerEnabledMask = {
            static_cast<uint32_t>(packed[84]),  static_cast<uint32_t>(packed[85]),
            static_cast<uint32_t>(packed[86]),  static_cast<uint32_t>(packed[87]),
            static_cast<uint32_t>(packed[88]),  static_cast<uint32_t>(packed[89]),
            static_cast<uint32_t>(packed[90]),  static_cast<uint32_t>(packed[91]),
            static_cast<uint32_t>(packed[92]),  static_cast<uint32_t>(packed[93]),
            static_cast<uint32_t>(packed[94]),  static_cast<uint32_t>(packed[95]),
            static_cast<uint32_t>(packed[96]),  static_cast<uint32_t>(packed[97]),
            static_cast<uint32_t>(packed[98]),  static_cast<uint32_t>(packed[99]),
        },
        .layerPan = {
            packed[100], packed[101], packed[102], packed[103],
            packed[104], packed[105], packed[106], packed[107],
            packed[108], packed[109], packed[110], packed[111],
            packed[112], packed[113], packed[114], packed[115],
        },
        .layerNotePan = {
            packed[116], packed[117], packed[118], packed[119],
            packed[120], packed[121], packed[122], packed[123],
            packed[124], packed[125], packed[126], packed[127],
            packed[128], packed[129], packed[130], packed[131],
        },
        .layerNoteAttrPan = {
            packed[132], packed[133], packed[134], packed[135],
            packed[136], packed[137], packed[138], packed[139],
            packed[140], packed[141], packed[142], packed[143],
            packed[144], packed[145], packed[146], packed[147],
        },
        .layerNoteTargetL = {
            packed[148], packed[149], packed[150], packed[151],
            packed[152], packed[153], packed[154], packed[155],
            packed[156], packed[157], packed[158], packed[159],
            packed[160], packed[161], packed[162], packed[163],
        },
        .layerNoteTargetR = {
            packed[164], packed[165], packed[166], packed[167],
            packed[168], packed[169], packed[170], packed[171],
            packed[172], packed[173], packed[174], packed[175],
            packed[176], packed[177], packed[178], packed[179],
        },
        .layerNoteCurL = {
            packed[180], packed[181], packed[182], packed[183],
            packed[184], packed[185], packed[186], packed[187],
            packed[188], packed[189], packed[190], packed[191],
            packed[192], packed[193], packed[194], packed[195],
        },
        .layerNoteCurR = {
            packed[196], packed[197], packed[198], packed[199],
            packed[200], packed[201], packed[202], packed[203],
            packed[204], packed[205], packed[206], packed[207],
            packed[208], packed[209], packed[210], packed[211],
        },
        .layerSampleMediumCodec = {
            packed[212], packed[213], packed[214], packed[215],
            packed[216], packed[217], packed[218], packed[219],
            packed[220], packed[221], packed[222], packed[223],
            packed[224], packed[225], packed[226], packed[227],
        },
    };

    sSnapshotMutex.unlock();
}

Snapshot getSnapshot() {
    std::lock_guard<std::mutex> lock(sSnapshotMutex);
    return sSnapshot;
}

std::vector<Event> getEventsSince(uint64_t sinceId, size_t limit) {
    std::vector<Event> result;
    result.reserve(limit);

    std::lock_guard<std::mutex> lock(sEventStore.mutex);

    for (size_t i = 0; i < sEventStore.size; i++) {
        const size_t idx = (sEventStore.head + i) % EVENT_CAPACITY;
        const Event& event = sEventStore.events[idx];

        if (event.id <= sinceId) {
            continue;
        }

        result.push_back(event);
        if (result.size() >= limit) {
            break;
        }
    }

    return result;
}

uint64_t droppedEventCount() {
    std::lock_guard<std::mutex> lock(sEventStore.mutex);
    return sEventStore.dropped;
}

uint64_t lastEventId() {
    std::lock_guard<std::mutex> lock(sEventStore.mutex);
    return sEventStore.nextId == 0 ? 0 : sEventStore.nextId - 1;
}

void startHttpServer() {
    setEnabled(true);
}

void registerTag(uint32_t tag, const char* name) {
    if (!name || tag == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(sTagRegistryMutex);
    sTagRegistry[tag] = name;
}

std::string getTagsJson() {
    std::lock_guard<std::mutex> lock(sTagRegistryMutex);
    std::ostringstream out;
    out << "{\"tags\":{";
    bool first = true;
    for (const auto& [tag, name] : sTagRegistry) {
        if (!first) {
            out << ',';
        }
        first = false;
        // Escape the name string
        out << '"' << tag << "\":\"";
        for (char ch : name) {
            if (ch == '"' || ch == '\\') out << '\\';
            out << ch;
        }
        out << '"';
    }
    out << "}}";
    return out.str();
}

void setStreamState(uint32_t resourceId, uint32_t trackNo, int32_t pos,
                    uint32_t sampleCount, uint32_t loopStart, uint32_t loopEnd, int32_t loopCount) {
    if (!isEnabled()) {
        return;
    }
    uint64_t key = (static_cast<uint64_t>(resourceId) << 8) | (trackNo & 0xFF);
    std::lock_guard<std::mutex> lock(sStreamMutex);
    sStreamStates[key] = StreamState {
        resourceId, trackNo, pos, sampleCount, loopStart, loopEnd, loopCount, nowMs()
    };
}

std::string getStreamsJson() {
    return streamsJson();
}

std::string buildAudioDebugHtml() {
    return kAudioDebugHtml;
}

void setMixerOpen(bool open) {
    if (!open) {
        // Clear all overrides so the game stops applying them immediately.
        std::lock_guard<std::mutex> lock(sMixMutex);
        for (auto& player : sMixOverrides) {
            for (auto& ov : player) {
                ov = MixOverride{};
            }
        }
    }
    sMixerOpen.store(open, std::memory_order_release);
}

bool isMixerOpen() {
    return sMixerOpen.load(std::memory_order_acquire);
}

void setMixOverride(uint32_t playerIndex, uint32_t channelIndex, int32_t pan, int32_t volMilli, int32_t reverb, bool muted) {
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    auto& ov = sMixOverrides[playerIndex][channelIndex];
    ov.active   = true;
    ov.pan      = pan;
    ov.volMilli = volMilli;
    ov.reverb   = reverb;
    ov.muted    = muted;
}

void clearMixOverride(uint32_t playerIndex, uint32_t channelIndex) {
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return;
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    sMixOverrides[playerIndex][channelIndex] = MixOverride{};
}

MixOverride getMixOverride(uint32_t playerIndex, uint32_t channelIndex) {
    if (!sMixerOpen.load(std::memory_order_acquire)) {
        return MixOverride{};
    }
    if (playerIndex >= MAX_PLAYERS || channelIndex >= MAX_MIX_CHANNELS) {
        return MixOverride{};
    }
    std::lock_guard<std::mutex> lock(sMixMutex);
    return sMixOverrides[playerIndex][channelIndex];
}

} // namespace Debug
