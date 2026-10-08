// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "wp/bluetooth.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

#include "wp/input.h"
#include "wp/ios.h"
#include "wp/memory.h"
#include "wp/wiimote.h"

namespace wp::bluetooth {

namespace {

constexpr const char* kDevicePath = "/dev/usb/oh1/57e/305";
constexpr uint32_t kVectorSize = 8;

constexpr uint32_t kControlMessage = 0;
constexpr uint32_t kBulkMessage = 1;
constexpr uint32_t kInterruptMessage = 2;

constexpr uint8_t kEndpointEvents = 0x81;
constexpr uint8_t kEndpointAclIn = 0x82;
constexpr uint8_t kEndpointAclOut = 0x02;

constexpr uint8_t kEventConnectionComplete = 0x03;
constexpr uint8_t kEventConnectionRequest = 0x04;
constexpr uint8_t kEventDisconnectionComplete = 0x05;
constexpr uint8_t kReasonRemoteTerminated = 0x13;
constexpr uint8_t kEventAuthenticationComplete = 0x06;
constexpr uint8_t kEventRemoteNameComplete = 0x07;
constexpr uint8_t kEventRemoteFeaturesComplete = 0x0B;
constexpr uint8_t kEventRemoteVersionComplete = 0x0C;
constexpr uint8_t kEventCommandComplete = 0x0E;
constexpr uint8_t kEventCommandStatus = 0x0F;
constexpr uint8_t kEventRoleChange = 0x12;
constexpr uint8_t kEventCompletedPackets = 0x13;
constexpr uint8_t kEventModeChange = 0x14;
constexpr uint8_t kEventReturnLinkKeys = 0x15;
constexpr uint8_t kEventClockOffsetComplete = 0x1C;
constexpr uint8_t kEventPacketTypeChanged = 0x1D;

constexpr uint16_t kCommandInquiry = 0x0401;
constexpr uint16_t kCommandCreateConnection = 0x0405;
constexpr uint16_t kCommandDisconnect = 0x0406;
constexpr uint16_t kCommandAcceptConnection = 0x0409;
constexpr uint16_t kCommandLinkKeyReply = 0x040B;
constexpr uint16_t kCommandLinkKeyNegativeReply = 0x040C;
constexpr uint16_t kCommandChangePacketType = 0x040F;
constexpr uint16_t kCommandAuthenticationRequest = 0x0411;
constexpr uint16_t kCommandRemoteNameRequest = 0x0419;
constexpr uint16_t kCommandReadRemoteFeatures = 0x041B;
constexpr uint16_t kCommandReadRemoteVersion = 0x041D;
constexpr uint16_t kCommandReadClockOffset = 0x041F;
constexpr uint16_t kCommandSniffMode = 0x0803;
constexpr uint16_t kCommandWriteLinkPolicy = 0x080D;
constexpr uint16_t kCommandReset = 0x0C03;
constexpr uint16_t kCommandReadStoredLinkKey = 0x0C0D;
constexpr uint16_t kCommandDeleteStoredLinkKey = 0x0C12;
constexpr uint16_t kCommandWriteScanEnable = 0x0C1A;
constexpr uint16_t kCommandWriteLinkSupervisionTimeout = 0x0C37;
constexpr uint16_t kCommandReadLocalVersion = 0x1001;
constexpr uint16_t kCommandReadLocalFeatures = 0x1003;
constexpr uint16_t kCommandReadBufferSize = 0x1005;
constexpr uint16_t kCommandReadBdAddr = 0x1009;

constexpr uint16_t kAclPacketSize = 339;
constexpr uint16_t kAclPacketCount = 10;
constexpr uint8_t kScoPacketSize = 64;
constexpr uint16_t kScoPacketCount = 0;
constexpr uint8_t kPageScanEnable = 0x02;

constexpr uint16_t kSignalChannel = 0x0001;
constexpr uint8_t kSignalReject = 0x01;
constexpr uint8_t kSignalConnectRequest = 0x02;
constexpr uint8_t kSignalConnectResponse = 0x03;
constexpr uint8_t kSignalConfigRequest = 0x04;
constexpr uint8_t kSignalConfigResponse = 0x05;
constexpr uint8_t kSignalDisconnectRequest = 0x06;
constexpr uint8_t kSignalDisconnectResponse = 0x07;
constexpr uint8_t kOptionMtu = 0x01;
constexpr uint16_t kDefaultMtu = 672;
constexpr uint16_t kRequestMtu = 185;
constexpr uint16_t kPsmSdp = 0x0001;
constexpr uint16_t kPsmHidControl = 0x0011;
constexpr uint16_t kPsmHidInterrupt = 0x0013;
constexpr uint8_t kHidSetReportOutput = 0x52;
constexpr uint8_t kHidDataOutput = 0xA2;
constexpr uint8_t kHidHandshakeSuccess = 0x00;

constexpr uint32_t kWiimoteCount = 4;
constexpr uint8_t kControllerAddress[6] = {0xff, 0x00, 0x79, 0x19, 0x02, 0x11};
constexpr uint8_t kWiimoteClass[3] = {0x00, 0x04, 0x48};
constexpr uint8_t kWiimoteFeatures[8] = {0xBC, 0x02, 0x04, 0x38, 0x08, 0x00, 0x00, 0x00};
constexpr const char* kWiimoteName = "Nintendo RVL-CNT-01";

struct Endpoint {
    uint32_t request = 0;
    uint32_t data = 0;
    uint32_t length = 0;
};

struct Completion {
    uint32_t request;
    int32_t result;
};

struct Channel {
    uint16_t psm = 0;
    uint16_t remote_cid = 0;
    uint16_t remote_mtu = 0;
    bool configuration_sent = false;
    bool complete = false;
};

enum class Baseband { Inactive, RequestConnection, Pending, Complete };

struct Wiimote {
    Baseband baseband = Baseband::Inactive;
    bool linking = false;
    bool host_ready = false;
    std::chrono::steady_clock::time_point link_deadline{};
    std::map<uint16_t, Channel> channels;
    uint32_t completed_packets = 0;
};

Endpoint g_events;
Endpoint g_acl_in;
std::deque<std::vector<uint8_t>> g_event_queue;
std::deque<std::vector<uint8_t>> g_acl_queue;
std::deque<Completion> g_completions;
std::array<Wiimote, kWiimoteCount> g_wiimotes;
uint8_t g_scan_enable = 0;
bool g_log = std::getenv("WP_LOG_BT") != nullptr;

uint32_t in_vector(uint32_t vectors, uint32_t index) {
    return rd32(vectors + index * kVectorSize);
}

uint32_t io_vector(uint32_t vectors, uint32_t input_count, uint32_t index) {
    return rd32(vectors + (input_count + index) * kVectorSize);
}

uint32_t io_vector_size(uint32_t vectors, uint32_t input_count, uint32_t index) {
    return rd32(vectors + (input_count + index) * kVectorSize + 4);
}

void put16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

uint16_t get16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

std::array<uint8_t, 6> wiimote_address(uint32_t index) {
    return {0x11, 0x02, 0x19, 0x79, 0x00, static_cast<uint8_t>(index)};
}

uint16_t connection_handle(uint32_t index) {
    return static_cast<uint16_t>(0x100 + index);
}

void put_address(std::vector<uint8_t>& out, uint32_t index) {
    std::array<uint8_t, 6> address = wiimote_address(index);
    out.insert(out.end(), address.begin(), address.end());
}

int wiimote_from_address(uint32_t address) {
    uint32_t index = rd8(address + 5);
    std::array<uint8_t, 6> expected = wiimote_address(index);
    for (uint32_t i = 0; i < 6; i++) {
        if (rd8(address + i) != expected[i]) {
            return -1;
        }
    }
    return index < kWiimoteCount ? static_cast<int>(index) : -1;
}

int wiimote_from_handle(uint16_t handle) {
    uint32_t index = handle & 0xFF;
    return (handle & 0x0F00) == 0x100 && index < kWiimoteCount ? static_cast<int>(index) : -1;
}

void deliver() {
    while (g_events.request != 0 && !g_event_queue.empty()) {
        const std::vector<uint8_t>& event = g_event_queue.front();
        uint32_t size = static_cast<uint32_t>(event.size());
        if (size > g_events.length) {
            size = g_events.length;
        }
        std::memcpy(host(g_events.data), event.data(), size);
        g_completions.push_back({g_events.request, static_cast<int32_t>(size)});
        g_events = Endpoint{};
        g_event_queue.pop_front();
    }
    if (g_acl_in.request != 0 && !g_acl_queue.empty() && g_event_queue.empty()) {
        const std::vector<uint8_t>& packet = g_acl_queue.front();
        uint32_t size = static_cast<uint32_t>(packet.size());
        if (size > g_acl_in.length) {
            size = g_acl_in.length;
        }
        std::memcpy(host(g_acl_in.data), packet.data(), size);
        g_completions.push_back({g_acl_in.request, static_cast<int32_t>(size)});
        g_acl_in = Endpoint{};
        g_acl_queue.pop_front();
    }
}

void queue_event(std::vector<uint8_t> event) {
    if (g_log) {
        std::fprintf(stderr, "BT event %02x length %u\n", event[0], static_cast<unsigned>(event.size()));
    }
    g_event_queue.push_back(std::move(event));
    deliver();
}

void event(uint8_t code, const std::vector<uint8_t>& parameters) {
    std::vector<uint8_t> packet = {code, static_cast<uint8_t>(parameters.size())};
    packet.insert(packet.end(), parameters.begin(), parameters.end());
    queue_event(std::move(packet));
}

void command_complete(uint16_t opcode, const std::vector<uint8_t>& parameters) {
    std::vector<uint8_t> body = {1};
    put16(body, opcode);
    body.insert(body.end(), parameters.begin(), parameters.end());
    event(kEventCommandComplete, body);
}

void command_status(uint16_t opcode, uint8_t status = 0) {
    std::vector<uint8_t> body = {status, 1};
    put16(body, opcode);
    event(kEventCommandStatus, body);
}

void send_acl(uint32_t index, uint16_t cid, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet;
    put16(packet, static_cast<uint16_t>(connection_handle(index) | (0x2 << 12)));
    put16(packet, static_cast<uint16_t>(payload.size() + 4));
    put16(packet, static_cast<uint16_t>(payload.size()));
    put16(packet, cid);
    packet.insert(packet.end(), payload.begin(), payload.end());
    g_acl_queue.push_back(std::move(packet));
    deliver();
}

void send_signal(uint32_t index, uint8_t ident, uint8_t code, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> command = {code, ident};
    put16(command, static_cast<uint16_t>(data.size()));
    command.insert(command.end(), data.begin(), data.end());
    if (g_log) {
        std::fprintf(stderr, "BT l2cap send code %02x ident %02x\n", code, ident);
    }
    send_acl(index, kSignalChannel, command);
}

Channel* channel_with_psm(Wiimote& wiimote, uint16_t psm) {
    for (auto& [cid, channel] : wiimote.channels) {
        if (channel.psm == psm) {
            return &channel;
        }
    }
    return nullptr;
}

uint16_t new_channel_id(const Wiimote& wiimote) {
    uint16_t cid = 0x40;
    while (wiimote.channels.count(cid)) {
        cid++;
    }
    return cid;
}

bool channel_ready(const Channel& channel) {
    return channel.remote_cid != 0 && channel.remote_mtu != 0 && channel.complete;
}

bool link_channel(uint32_t index, uint16_t psm) {
    Wiimote& wiimote = g_wiimotes[index];
    Channel* channel = channel_with_psm(wiimote, psm);
    if (channel == nullptr) {
        uint16_t cid = new_channel_id(wiimote);
        wiimote.channels[cid].psm = psm;
        std::vector<uint8_t> request;
        put16(request, psm);
        put16(request, cid);
        send_signal(index, kSignalConnectRequest, kSignalConnectRequest, request);
        return false;
    }
    return channel_ready(*channel);
}

void send_interrupt(uint32_t index, const std::vector<uint8_t>& frame) {
    Channel* channel = channel_with_psm(g_wiimotes[index], kPsmHidInterrupt);
    if (channel == nullptr || channel->remote_cid == 0) {
        return;
    }
    send_acl(index, channel->remote_cid, frame);
}

void signal(uint32_t index, const uint8_t* data, uint32_t size) {
    Wiimote& wiimote = g_wiimotes[index];
    while (size >= 4) {
        uint8_t code = data[0];
        uint8_t ident = data[1];
        uint16_t length = get16(data + 2);
        const uint8_t* body = data + 4;
        if (4u + length > size) {
            break;
        }
        if (g_log) {
            std::fprintf(stderr, "BT l2cap receive code %02x ident %02x length %u\n", code, ident, length);
        }
        switch (code) {
        case kSignalConnectRequest: {
            uint16_t psm = get16(body);
            uint16_t scid = get16(body + 2);
            std::vector<uint8_t> response;
            if (channel_with_psm(wiimote, psm) != nullptr) {
                put16(response, 0);
                put16(response, scid);
                put16(response, 0x0004);
                put16(response, 0);
            } else {
                uint16_t cid = new_channel_id(wiimote);
                Channel& channel = wiimote.channels[cid];
                channel.psm = psm;
                channel.remote_cid = scid;
                put16(response, cid);
                put16(response, scid);
                put16(response, 0);
                put16(response, 0);
            }
            send_signal(index, ident, kSignalConnectResponse, response);
            break;
        }
        case kSignalConnectResponse: {
            uint16_t dcid = get16(body);
            uint16_t scid = get16(body + 2);
            uint16_t result = get16(body + 4);
            if (g_log) {
                std::fprintf(stderr, "BT l2cap connect response dcid %04x scid %04x result %04x status %04x\n", dcid, scid, result, get16(body + 6));
            }
            auto it = wiimote.channels.find(scid);
            if (it != wiimote.channels.end() && result == 0) {
                it->second.remote_cid = dcid;
            }
            break;
        }
        case kSignalConfigRequest: {
            uint16_t dcid = get16(body);
            auto it = wiimote.channels.find(dcid);
            if (it == wiimote.channels.end()) {
                break;
            }
            std::vector<uint8_t> response;
            put16(response, it->second.remote_cid);
            put16(response, 0);
            put16(response, 0);
            uint16_t mtu = kDefaultMtu;
            uint32_t offset = 4;
            while (offset + 2 <= length) {
                uint8_t type = body[offset];
                uint8_t option_length = body[offset + 1];
                if (type == kOptionMtu && option_length == 2) {
                    mtu = get16(body + offset + 2);
                }
                response.insert(response.end(), body + offset, body + offset + 2 + option_length);
                offset += 2 + option_length;
            }
            it->second.remote_mtu = mtu;
            send_signal(index, ident, kSignalConfigResponse, response);
            break;
        }
        case kSignalConfigResponse: {
            uint16_t scid = get16(body);
            auto it = wiimote.channels.find(scid);
            if (it != wiimote.channels.end()) {
                it->second.complete = true;
            }
            break;
        }
        case kSignalDisconnectRequest: {
            uint16_t dcid = get16(body);
            uint16_t scid = get16(body + 2);
            wiimote.channels.erase(dcid);
            std::vector<uint8_t> response;
            put16(response, dcid);
            put16(response, scid);
            send_signal(index, ident, kSignalDisconnectResponse, response);
            break;
        }
        case kSignalReject:
        default:
            std::fprintf(stderr, "BT l2cap signal %02x not handled\n", code);
            break;
        }
        data += 4 + length;
        size -= 4 + length;
    }
}

void receive_acl(uint32_t data) {
    uint16_t handle = static_cast<uint16_t>(rd8(data) | (rd8(data + 1) << 8)) & 0x0FFF;
    uint16_t length = static_cast<uint16_t>(rd8(data + 2) | (rd8(data + 3) << 8));
    int index = wiimote_from_handle(handle);
    if (index < 0 || length < 4) {
        return;
    }
    g_wiimotes[index].completed_packets++;
    std::vector<uint8_t> packet(length);
    std::memcpy(packet.data(), host(data + 4), length);
    uint16_t l2cap_length = get16(packet.data());
    uint16_t cid = get16(packet.data() + 2);
    const uint8_t* payload = packet.data() + 4;
    uint32_t payload_size = std::min<uint32_t>(l2cap_length, length - 4u);
    if (cid == kSignalChannel) {
        signal(static_cast<uint32_t>(index), payload, payload_size);
        return;
    }
    Wiimote& wiimote = g_wiimotes[index];
    auto it = wiimote.channels.find(cid);
    if (it == wiimote.channels.end() || payload_size == 0) {
        return;
    }
    uint32_t wiimote_index = static_cast<uint32_t>(index);
    auto sender = [wiimote_index](const std::vector<uint8_t>& frame) { send_interrupt(wiimote_index, frame); };
    if (it->second.psm == kPsmHidInterrupt && payload[0] == kHidDataOutput) {
        wiimote::output_report(wiimote_index, payload + 1, payload_size - 1, sender);
    } else if (it->second.psm == kPsmHidControl && payload[0] == kHidSetReportOutput) {
        send_acl(wiimote_index, it->second.remote_cid, {kHidHandshakeSuccess});
        wiimote::output_report(wiimote_index, payload + 1, payload_size - 1, sender);
    } else {
        std::fprintf(stderr, "BT unhandled data on psm %04x type %02x\n", it->second.psm, payload[0]);
    }
}

void execute_command(uint32_t data) {
    uint16_t opcode = static_cast<uint16_t>(rd8(data) | (rd8(data + 1) << 8));
    uint32_t parameters = data + 3;
    if (g_log) {
        std::fprintf(stderr, "BT command %04x\n", opcode);
    }
    switch (opcode) {
    case kCommandReadLocalVersion: {
        std::vector<uint8_t> reply = {0x00, 0x03};
        put16(reply, 0x40a7);
        reply.push_back(0x03);
        put16(reply, 0x000F);
        put16(reply, 0x430e);
        command_complete(opcode, reply);
        break;
    }
    case kCommandReadLocalFeatures:
        command_complete(opcode, {0x00, 0xFF, 0xFF, 0x8D, 0xFE, 0x9B, 0xF9, 0x00, 0x80});
        break;
    case kCommandReadBufferSize: {
        std::vector<uint8_t> reply = {0x00};
        put16(reply, kAclPacketSize);
        reply.push_back(kScoPacketSize);
        put16(reply, kAclPacketCount);
        put16(reply, kScoPacketCount);
        command_complete(opcode, reply);
        break;
    }
    case kCommandReadBdAddr: {
        std::vector<uint8_t> reply = {0x00};
        for (uint8_t byte : kControllerAddress) {
            reply.push_back(byte);
        }
        command_complete(opcode, reply);
        break;
    }
    case kCommandReadStoredLinkKey: {
        bool read_all = rd8(parameters + 6) == 1;
        uint8_t count = read_all ? static_cast<uint8_t>(kWiimoteCount) : 0;
        std::vector<uint8_t> keys = {count};
        for (uint32_t i = 0; i < count; i++) {
            put_address(keys, i);
            keys.insert(keys.end(), 16, static_cast<uint8_t>(0xa0 + i));
        }
        event(kEventReturnLinkKeys, keys);
        std::vector<uint8_t> reply = {0x00};
        put16(reply, 255);
        put16(reply, count);
        command_complete(opcode, reply);
        break;
    }
    case kCommandDeleteStoredLinkKey: {
        std::vector<uint8_t> reply = {0x00};
        put16(reply, 0);
        command_complete(opcode, reply);
        break;
    }
    case kCommandWriteScanEnable:
        g_scan_enable = rd8(parameters);
        command_complete(opcode, {0x00});
        break;
    case kCommandWriteLinkSupervisionTimeout: {
        int supervised = wiimote_from_handle(static_cast<uint16_t>(rd8(parameters) | (rd8(parameters + 1) << 8)));
        if (supervised >= 0) {
            g_wiimotes[supervised].host_ready = true;
        }
        std::vector<uint8_t> reply = {0x00, rd8(parameters), rd8(parameters + 1)};
        command_complete(opcode, reply);
        break;
    }
    case kCommandInquiry:
        command_status(opcode);
        break;
    case kCommandCreateConnection: {
        command_status(opcode);
        int index = wiimote_from_address(parameters);
        bool ok = index >= 0 && input::connected(static_cast<uint32_t>(index)) && (g_scan_enable & kPageScanEnable);
        if (ok) {
            g_wiimotes[index].baseband = Baseband::Complete;
        }
        std::vector<uint8_t> body = {static_cast<uint8_t>(ok ? 0x00 : 0x08)};
        put16(body, connection_handle(index < 0 ? 0 : static_cast<uint32_t>(index)));
        for (uint32_t i = 0; i < 6; i++) {
            body.push_back(rd8(parameters + i));
        }
        body.push_back(0x01);
        body.push_back(0x00);
        event(kEventConnectionComplete, body);
        break;
    }
    case kCommandAcceptConnection: {
        command_status(opcode);
        int index = wiimote_from_address(parameters);
        uint8_t role = rd8(parameters + 6);
        bool ok = index >= 0 && input::connected(static_cast<uint32_t>(index)) && (g_scan_enable & kPageScanEnable);
        if (ok) {
            g_wiimotes[index].baseband = Baseband::Complete;
            g_wiimotes[index].linking = true;
            g_wiimotes[index].host_ready = false;
            g_wiimotes[index].link_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            if (role == 0) {
                std::vector<uint8_t> change = {0x00};
                put_address(change, static_cast<uint32_t>(index));
                change.push_back(0x00);
                event(kEventRoleChange, change);
            }
        }
        std::vector<uint8_t> body = {static_cast<uint8_t>(ok ? 0x00 : 0x08)};
        put16(body, connection_handle(index < 0 ? 0 : static_cast<uint32_t>(index)));
        for (uint32_t i = 0; i < 6; i++) {
            body.push_back(rd8(parameters + i));
        }
        body.push_back(0x01);
        body.push_back(0x00);
        event(kEventConnectionComplete, body);
        break;
    }
    case kCommandDisconnect: {
        uint16_t handle = static_cast<uint16_t>(rd8(parameters) | (rd8(parameters + 1) << 8));
        uint8_t reason = rd8(parameters + 2);
        command_status(opcode);
        std::vector<uint8_t> body = {0x00};
        put16(body, handle);
        body.push_back(reason);
        event(kEventDisconnectionComplete, body);
        int index = wiimote_from_handle(handle);
        if (index >= 0) {
            g_wiimotes[index] = Wiimote{};
            wiimote::reset(static_cast<uint32_t>(index));
        }
        break;
    }
    case kCommandLinkKeyReply:
    case kCommandLinkKeyNegativeReply: {
        std::vector<uint8_t> reply = {0x00};
        for (uint32_t i = 0; i < 6; i++) {
            reply.push_back(rd8(parameters + i));
        }
        command_complete(opcode, reply);
        break;
    }
    case kCommandChangePacketType: {
        command_status(opcode);
        std::vector<uint8_t> body = {0x00, rd8(parameters), rd8(parameters + 1), rd8(parameters + 2), rd8(parameters + 3)};
        event(kEventPacketTypeChanged, body);
        break;
    }
    case kCommandAuthenticationRequest: {
        command_status(opcode);
        event(kEventAuthenticationComplete, {0x00, rd8(parameters), rd8(parameters + 1)});
        break;
    }
    case kCommandRemoteNameRequest: {
        command_status(opcode);
        int index = wiimote_from_address(parameters);
        if (index >= 0) {
            std::vector<uint8_t> body = {0x00};
            put_address(body, static_cast<uint32_t>(index));
            std::vector<uint8_t> name(248, 0);
            std::memcpy(name.data(), kWiimoteName, std::strlen(kWiimoteName));
            body.insert(body.end(), name.begin(), name.end());
            event(kEventRemoteNameComplete, body);
        }
        break;
    }
    case kCommandReadRemoteFeatures: {
        command_status(opcode);
        std::vector<uint8_t> body = {0x00, rd8(parameters), rd8(parameters + 1)};
        for (uint8_t byte : kWiimoteFeatures) {
            body.push_back(byte);
        }
        event(kEventRemoteFeaturesComplete, body);
        break;
    }
    case kCommandReadRemoteVersion: {
        command_status(opcode);
        std::vector<uint8_t> body = {0x00, rd8(parameters), rd8(parameters + 1), 0x02};
        put16(body, 0x000F);
        put16(body, 0x0229);
        event(kEventRemoteVersionComplete, body);
        break;
    }
    case kCommandReadClockOffset: {
        command_status(opcode);
        std::vector<uint8_t> body = {0x00, rd8(parameters), rd8(parameters + 1)};
        put16(body, 0x3818);
        event(kEventClockOffsetComplete, body);
        break;
    }
    case kCommandSniffMode: {
        command_status(opcode);
        std::vector<uint8_t> body = {0x00, rd8(parameters), rd8(parameters + 1), 0x02, rd8(parameters + 2), rd8(parameters + 3)};
        event(kEventModeChange, body);
        break;
    }
    case kCommandWriteLinkPolicy:
        command_status(opcode);
        break;
    case kCommandReset:
    default:
        command_complete(opcode, {0x00});
        break;
    }
}

void update_wiimote(uint32_t index) {
    Wiimote& wiimote = g_wiimotes[index];
    if (wiimote.baseband != Baseband::Inactive && !input::connected(index)) {
        if (wiimote.baseband == Baseband::Complete) {
            std::vector<uint8_t> body = {0x00};
            put16(body, connection_handle(index));
            body.push_back(kReasonRemoteTerminated);
            event(kEventDisconnectionComplete, body);
        }
        wiimote = Wiimote{};
        wiimote::reset(index);
        return;
    }
    if (wiimote.baseband == Baseband::Inactive && input::connected(index) && input::wakes_remote(index)) {
        wiimote.baseband = Baseband::RequestConnection;
    }
    if (wiimote.baseband == Baseband::RequestConnection && (g_scan_enable & kPageScanEnable)) {
        std::vector<uint8_t> body;
        put_address(body, index);
        body.insert(body.end(), kWiimoteClass, kWiimoteClass + 3);
        body.push_back(0x01);
        event(kEventConnectionRequest, body);
        wiimote.baseband = Baseband::Pending;
    }
    if (wiimote.baseband != Baseband::Complete) {
        return;
    }
    for (auto& [cid, channel] : wiimote.channels) {
        if (channel.remote_cid != 0 && !channel.configuration_sent) {
            channel.configuration_sent = true;
            std::vector<uint8_t> request;
            put16(request, channel.remote_cid);
            put16(request, 0);
            request.push_back(kOptionMtu);
            request.push_back(2);
            put16(request, kRequestMtu);
            send_signal(index, kSignalConfigRequest, kSignalConfigRequest, request);
        }
    }
    bool may_link = wiimote.host_ready || std::chrono::steady_clock::now() >= wiimote.link_deadline;
    if (wiimote.linking && may_link && link_channel(index, kPsmHidControl) && link_channel(index, kPsmHidInterrupt)) {
        wiimote.linking = false;
    }
    Channel* interrupt = channel_with_psm(wiimote, kPsmHidInterrupt);
    if (interrupt != nullptr && channel_ready(*interrupt)) {
        wiimote::update(index, [index](const std::vector<uint8_t>& frame) { send_interrupt(index, frame); });
    }
}

void send_completed_packets() {
    std::vector<uint8_t> body = {static_cast<uint8_t>(kWiimoteCount)};
    uint32_t total = 0;
    for (uint32_t i = 0; i < kWiimoteCount; i++) {
        put16(body, connection_handle(i));
        put16(body, static_cast<uint16_t>(g_wiimotes[i].completed_packets));
        total += g_wiimotes[i].completed_packets;
        g_wiimotes[i].completed_packets = 0;
    }
    if (total != 0) {
        event(kEventCompletedPackets, body);
    }
}

bool g_started = false;

void start() {
    if (g_started) {
        return;
    }
    g_started = true;
    for (uint32_t i = 0; i < kWiimoteCount; i++) {
        wiimote::reset(i);
        if (input::connected(i)) {
            g_wiimotes[i].baseband = Baseband::RequestConnection;
        }
    }
}

}

bool handles(const std::string& path) {
    return path == kDevicePath;
}

int32_t ioctlv(uint32_t request, uint32_t command, uint32_t input_count, uint32_t, uint32_t vectors) {
    start();
    switch (command) {
    case kControlMessage: {
        uint32_t data = io_vector(vectors, input_count, 0);
        uint32_t length = rd8(in_vector(vectors, 4)) | (rd8(in_vector(vectors, 4) + 1) << 8);
        execute_command(data);
        return static_cast<int32_t>(length);
    }
    case kBulkMessage: {
        uint8_t endpoint = rd8(in_vector(vectors, 0));
        if (endpoint == kEndpointAclIn) {
            g_acl_in = {request, io_vector(vectors, input_count, 0), io_vector_size(vectors, input_count, 0)};
            deliver();
            return ios::kDeferred;
        }
        if (endpoint == kEndpointAclOut) {
            receive_acl(io_vector(vectors, input_count, 0));
            return static_cast<int32_t>(rd16(in_vector(vectors, 1)));
        }
        break;
    }
    case kInterruptMessage: {
        uint8_t endpoint = rd8(in_vector(vectors, 0));
        if (endpoint == kEndpointEvents) {
            g_events = {request, io_vector(vectors, input_count, 0), io_vector_size(vectors, input_count, 0)};
            deliver();
            return ios::kDeferred;
        }
        break;
    }
    default:
        break;
    }
    std::fprintf(stderr, "BT unhandled ioctlv %u\n", command);
    return 0;
}

void close() {
    g_events = Endpoint{};
    g_acl_in = Endpoint{};
    g_event_queue.clear();
    g_acl_queue.clear();
    g_scan_enable = 0;
}

void update() {
    if (g_started) {
        for (uint32_t i = 0; i < kWiimoteCount; i++) {
            update_wiimote(i);
        }
        send_completed_packets();
    }
    deliver();
}

bool take_completion(uint32_t& request, int32_t& result) {
    if (g_completions.empty()) {
        return false;
    }
    request = g_completions.front().request;
    result = g_completions.front().result;
    g_completions.pop_front();
    return true;
}

}
