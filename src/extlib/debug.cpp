#include <extlib/debug.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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

void handleClient(SocketHandle client) {
    char reqBuf[4096];
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
    if (line.rfind("GET ", 0) != 0) {
        sendHttp(client, "405 Method Not Allowed", "text/plain; charset=utf-8", "Only GET supported");
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
        },
        .chPan = {
            packed[24], packed[25], packed[26], packed[27],
        },
        .chVolMilli = {
            packed[28], packed[29], packed[30], packed[31],
        },
        .layerEnabledMask = {
            static_cast<uint32_t>(packed[32]), static_cast<uint32_t>(packed[33]),
            static_cast<uint32_t>(packed[34]), static_cast<uint32_t>(packed[35]),
        },
        .layerPan = {
            packed[36], packed[37], packed[38], packed[39], packed[40], packed[41], packed[42], packed[43],
            packed[44], packed[45], packed[46], packed[47], packed[48], packed[49], packed[50], packed[51],
        },
        .layerNotePan = {
            packed[52], packed[53], packed[54], packed[55], packed[56], packed[57], packed[58], packed[59],
            packed[60], packed[61], packed[62], packed[63], packed[64], packed[65], packed[66], packed[67],
        },
        .layerNoteAttrPan = {
            packed[68], packed[69], packed[70], packed[71], packed[72], packed[73], packed[74], packed[75],
            packed[76], packed[77], packed[78], packed[79], packed[80], packed[81], packed[82], packed[83],
        },
        .layerNoteTargetL = {
            packed[84], packed[85], packed[86], packed[87], packed[88], packed[89], packed[90], packed[91],
            packed[92], packed[93], packed[94], packed[95], packed[96], packed[97], packed[98], packed[99],
        },
        .layerNoteTargetR = {
            packed[100], packed[101], packed[102], packed[103], packed[104], packed[105], packed[106], packed[107],
            packed[108], packed[109], packed[110], packed[111], packed[112], packed[113], packed[114], packed[115],
        },
        .layerNoteCurL = {
            packed[116], packed[117], packed[118], packed[119], packed[120], packed[121], packed[122], packed[123],
            packed[124], packed[125], packed[126], packed[127], packed[128], packed[129], packed[130], packed[131],
        },
        .layerNoteCurR = {
            packed[132], packed[133], packed[134], packed[135], packed[136], packed[137], packed[138], packed[139],
            packed[140], packed[141], packed[142], packed[143], packed[144], packed[145], packed[146], packed[147],
        },
        .layerSampleMediumCodec = {
            packed[148], packed[149], packed[150], packed[151], packed[152], packed[153], packed[154], packed[155],
            packed[156], packed[157], packed[158], packed[159], packed[160], packed[161], packed[162], packed[163],
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

std::string buildAudioDebugHtml() {
    return R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Audio API Debug</title>
  <style>
    :root {
      --bg0: #0f1a22;
      --bg1: #152737;
      --card: #10212fdd;
      --line: #2a4b61;
      --text: #dff1ff;
      --muted: #9bbad0;
      --accent: #55d6be;
      --warn: #ffbe5c;
      --bad: #ff6a6a;
      --mono: "JetBrains Mono", "Cascadia Mono", "SFMono-Regular", Menlo, monospace;
      --sans: "IBM Plex Sans", "Segoe UI", sans-serif;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      font-family: var(--sans);
      color: var(--text);
      background: radial-gradient(1200px 500px at 10% 0%, #244660, transparent), linear-gradient(160deg, var(--bg0), var(--bg1));
      min-height: 100vh;
    }
    .wrap { max-width: 1200px; margin: 0 auto; padding: 20px; }
    h1 { margin: 0 0 12px; font-size: 24px; letter-spacing: 0.02em; }
    .status {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 10px;
      margin-bottom: 14px;
    }
    .card {
      background: var(--card);
      border: 1px solid var(--line);
      border-radius: 10px;
      padding: 10px 12px;
      backdrop-filter: blur(3px);
    }
    .k { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: 0.07em; }
    .v { font-family: var(--mono); font-size: 18px; margin-top: 4px; }
    .good { color: var(--accent); }
    .warn { color: var(--warn); }
    .bad { color: var(--bad); }
    table {
      width: 100%;
      border-collapse: collapse;
      font-family: var(--mono);
      font-size: 12px;
    }
    th, td { padding: 7px 8px; border-bottom: 1px solid #1f3b50; text-align: left; }
    th { color: var(--muted); font-weight: 600; }
    .grid { display: grid; grid-template-columns: 1fr; gap: 12px; }
    @media (min-width: 980px) { .grid { grid-template-columns: 1fr 1fr; } }
    .events { max-height: 420px; overflow: auto; }
    .pill { display: inline-block; border: 1px solid var(--line); border-radius: 999px; padding: 2px 8px; font-size: 11px; color: var(--muted); }
    .chviz { min-width: 280px; display: grid; gap: 3px; }
    .chrow { display: grid; grid-template-columns: 26px 1fr 52px; align-items: center; gap: 6px; }
    .chid { color: var(--muted); font-size: 11px; }
    .pantrack { position: relative; height: 8px; border-radius: 999px; background: #1b3548; border: 1px solid #2a4b61; }
    .pancenter { position: absolute; left: 50%; top: -1px; bottom: -1px; width: 1px; background: #88a7bd; opacity: 0.8; }
    .panpos { position: absolute; top: 0; bottom: 0; width: 2px; background: #7fe3d1; }
    .voltxt { font-size: 11px; color: var(--text); text-align: right; }
    .is-off .pantrack { opacity: 0.35; }
    .is-off .voltxt { color: var(--muted); }
  </style>
</head>
<body>
  <div class="wrap">
    <h1>Audio API Live Debug</h1>
    <div class="status">
      <div class="card"><div class="k">Connection</div><div id="conn" class="v">connecting...</div></div>
      <div class="card"><div class="k">Init Phase</div><div id="phase" class="v">-</div></div>
      <div class="card"><div class="k">Last Event ID</div><div id="lastId" class="v">0</div></div>
      <div class="card"><div class="k">Dropped Events</div><div id="dropped" class="v">0</div></div>
      <div class="card"><div class="k">Main/Sub/Fanfare</div><div id="quickSeq" class="v">-</div></div>
    </div>

    <div class="grid">
      <div class="card">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
          <span class="pill">/snapshot</span>
          <span id="snapTs" class="k">ts: -</span>
        </div>
        <table>
          <thead><tr><th>Player</th><th>Role</th><th>SeqId</th><th>En</th><th>Ch</th><th>Pan L/C/R(avg)</th><th>Notes A/D/R</th><th>Ch0..3 pan@vol</th><th>L0..3 pan/notePan</th><th>L0..3 tgtLR/curLR</th><th>Seq IO</th></tr></thead>
          <tbody id="players"></tbody>
        </table>
      </div>

      <div class="card">
        <div style="display:flex; justify-content:space-between; align-items:center; margin-bottom:8px;">
          <span class="pill">/events</span>
          <span class="k">latest first</span>
        </div>
        <div class="events">
          <table>
            <thead><tr><th>ID</th><th>Tag</th><th>a</th><th>b</th><th>c</th><th>d</th><th>ts(ms)</th></tr></thead>
            <tbody id="events"></tbody>
          </table>
        </div>
      </div>
    </div>
  </div>

  <script>
    const tags = {
      1: 'QUEUE_EXT_CMD',
      2: 'PROCESS_SEQ_CMD',
      3: 'START_SEQUENCE',
      4: 'STOP_SEQUENCE',
      5: 'PLAY_SCENE_SEQUENCE',
      6: 'PLAY_SUB_BGM',
      7: 'PLAY_FANFARE',
      8: 'PLAY_OBJ_BGM',
      9: 'PLAY_CUTSCENE_SEQUENCE',
      10: 'START_MORNING_SEQUENCE',
      11: 'BGM_BLEND_INTENT',
      12: 'SEQ_IO_CHANGE',
      13: 'MUTE_ALL_EXCEPT_SYS_OCA',
      14: 'SET_SFX_VOL_EXCEPT',
      15: 'SFX_VOL_TRANSITION',
      16: 'MUTE_SFX_AMBIENCE_ONLY',
      17: 'START_SFX_PLAYER',
      18: 'MUTE_SFX_AMBIENCE_SYS_OCA',
      19: 'SET_SPEC',
      20: 'RESET_HEAP_STEP3',
      21: 'SFX_MUTE_BANKS_BEFORE',
      22: 'SFX_MUTE_BANKS_AFTER',
      23: 'SFX_BANK_MASK',
    };

    let since = 0;
    const eventRows = [];

    async function j(url) {
      const res = await fetch(url, { cache: 'no-store' });
      if (!res.ok) throw new Error(url + ' ' + res.status);
      return res.json();
    }

    function setConn(ok) {
      const el = document.getElementById('conn');
      el.textContent = ok ? 'online' : 'offline';
      el.className = 'v ' + (ok ? 'good' : 'bad');
    }

    function renderEvents() {
      const tbody = document.getElementById('events');
      tbody.innerHTML = eventRows.slice(-400).reverse().map(e =>
        `<tr><td>${e.id}</td><td>${tags[e.tag] || e.tag}</td><td>${e.a}</td><td>${e.b}</td><td>${e.c}</td><td>${e.d}</td><td>${e.ts_ms}</td></tr>`
      ).join('');
    }

    function renderPlayers(snapshot) {
      const tbody = document.getElementById('players');
      tbody.innerHTML = snapshot.players.map(p => {
        const io = (p.seq_io || []).join('/');
        const pan = `${p.pan_left_count}/${p.pan_center_count}/${p.pan_right_count} (${p.pan_average})`;
        const notes = `${p.notes_active}/${p.notes_decaying}/${p.notes_releasing}`;
        const chPan = p.ch_pan || [];
        const chVol = p.ch_vol_milli || [];
        const chEn = p.ch_enabled || [];
        const layerPan = p.layer_pan || [];
        const layerNotePan = p.layer_note_pan || [];
        const noteTL = p.layer_note_target_l || [];
        const noteTR = p.layer_note_target_r || [];
        const noteCL = p.layer_note_cur_l || [];
        const noteCR = p.layer_note_cur_r || [];
        const first4 = [0, 1, 2, 3].map(i => {
          const enabled = !!chEn[i];
          const panValue = Number.isFinite(chPan[i]) ? chPan[i] : -1;
          const vol = (Number.isFinite(chVol[i]) ? chVol[i] : 0) / 1000;
          const panPct = panValue < 0 ? 50 : Math.max(0, Math.min(127, panValue)) / 127 * 100;
          return `<div class="chrow ${enabled ? '' : 'is-off'}"><div class="chid">ch${i}</div><div class="pantrack"><span class="pancenter"></span><span class="panpos" style="left:${panPct}%"></span></div><div class="voltxt">${enabled ? vol.toFixed(2) : '-'}</div></div>`;
        }).join('');
        const layers = [0, 1, 2, 3].map(ch => {
          const base = ch * 4;
          const row = [0, 1, 2, 3].map(l => {
            const lp = layerPan[base + l];
            const lnp = layerNotePan[base + l];
            return lp < 0 ? '-' : `${lp}/${lnp}`;
          }).join(' ');
          return `<div class="chrow"><div class="chid">ch${ch}</div><div style="grid-column: span 2; font-size:11px; color:var(--muted)">${row}</div></div>`;
        }).join('');
        const vols = [0, 1, 2, 3].map(ch => {
          const base = ch * 4;
          const row = [0, 1, 2, 3].map(l => {
            const tl = noteTL[base + l];
            const tr = noteTR[base + l];
            const cl = noteCL[base + l];
            const cr = noteCR[base + l];
            return tl < 0 ? '-' : `${tl}/${tr}|${cl}/${cr}`;
          }).join(' ');
          return `<div class="chrow"><div class="chid">ch${ch}</div><div style="grid-column: span 2; font-size:11px; color:var(--muted)">${row}</div></div>`;
        }).join('');
        return `<tr><td>${p.player}</td><td>${p.role}</td><td>${p.seq_id}</td><td>${p.enabled}</td><td>${p.active_channel_count}</td><td>${pan}</td><td>${notes}</td><td><div class="chviz">${first4}</div></td><td><div class="chviz">${layers}</div></td><td><div class="chviz">${vols}</div></td><td>${io}</td></tr>`;
      }).join('');
    }

    async function tick() {
      try {
        const [snapshot, events] = await Promise.all([
          j('/snapshot'),
          j('/events?since=' + since + '&limit=300')
        ]);

        setConn(true);

        document.getElementById('phase').textContent = snapshot.init_phase;
        document.getElementById('snapTs').textContent = 'ts: ' + snapshot.ts_ms;
        document.getElementById('quickSeq').textContent = `${snapshot.main_seq_id} / ${snapshot.sub_seq_id} / ${snapshot.fanfare_seq_id}`;
        document.getElementById('lastId').textContent = events.last_event_id;
        document.getElementById('dropped').textContent = events.dropped_total;
        document.getElementById('dropped').className = 'v ' + (events.dropped_total > 0 ? 'warn' : 'good');

        renderPlayers(snapshot);

        if (events.events.length) {
          since = events.events[events.events.length - 1].id;
          eventRows.push(...events.events);
          if (eventRows.length > 3000) eventRows.splice(0, eventRows.length - 3000);
          renderEvents();
        }
      } catch (err) {
        setConn(false);
      }
    }

    setInterval(tick, 300);
    tick();
  </script>
</body>
</html>)HTML";
}

} // namespace Debug
