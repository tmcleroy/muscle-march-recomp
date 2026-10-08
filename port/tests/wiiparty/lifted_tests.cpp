#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <random>
#include <set>
#include <vector>

#include <bits/stl_tree.h>

#include "functions.h"
#include "wp/cpu.h"

namespace {

constexpr uint32_t kObject = 0x80100000;
constexpr uint32_t kInner = 0x80101000;
constexpr uint32_t kText = 0x80102000;
constexpr uint32_t kOutputOffset = 0xBC;
constexpr uint32_t kOutputSize = 0x800;

void reference_quote_scan(uint32_t object) {
    uint32_t text = wp::rd32(wp::rd32(object + 4) + 8);
    uint32_t index = 0;
    uint32_t slot = 0;
    uint32_t stride = 0;
    bool open = false;
    for (uint32_t p = text; wp::rd16(p) != 0; p += 2) {
        if (wp::rd16(p) == 0x22) {
            if (open) {
                open = false;
            } else {
                uint32_t address = slot * 0x18 + stride + object + kOutputOffset;
                slot++;
                open = true;
                wp::wr32(address, text + (index + 1) * 2);
                if (slot == 10) {
                    slot = 0;
                    stride += 4;
                }
            }
        }
        index++;
    }
}

std::vector<uint8_t> snapshot_output() {
    const uint8_t* start = wp::host(kObject + kOutputOffset);
    return std::vector<uint8_t>(start, start + kOutputSize);
}

void write_text(const std::vector<uint16_t>& characters) {
    uint32_t address = kText;
    for (uint16_t value : characters) {
        wp::wr16(address, value);
        address += 2;
    }
    wp::wr16(address, 0);
}

bool run_quote_scan(const std::vector<uint16_t>& characters) {
    std::memset(wp::host(kObject), 0, 0x1000);
    wp::wr32(kObject + 4, kInner);
    wp::wr32(kInner + 8, kText);
    write_text(characters);

    wp::Cpu c{};
    c.r[3] = kObject;
    f_8007b190(c);
    std::vector<uint8_t> lifted = snapshot_output();

    bool expects_output = std::count(characters.begin(), characters.end(), 0x22) > 0;
    bool has_output = std::any_of(lifted.begin(), lifted.end(), [](uint8_t byte) { return byte != 0; });
    if (expects_output != has_output) {
        return false;
    }

    std::memset(wp::host(kObject + kOutputOffset), 0, kOutputSize);
    reference_quote_scan(kObject);
    return lifted == snapshot_output();
}

std::vector<uint16_t> quoted_text(int pairs) {
    std::vector<uint16_t> characters;
    for (int i = 0; i < pairs; i++) {
        characters.push_back('a' + i % 26);
        characters.push_back(0x22);
        characters.push_back('x');
        characters.push_back('y');
        characters.push_back(0x22);
    }
    return characters;
}

constexpr uint32_t kTreeHeader = 0x80200000;
constexpr uint32_t kTreeNodes = 0x80200100;
constexpr uint32_t kNodeStride = 0x20;
constexpr uint32_t kKeyOffset = 0x10;
constexpr uint32_t kRedBit = 1;

struct HostNode : std::_Rb_tree_node_base {
    int key;
};

uint32_t guest_address(const std::map<const std::_Rb_tree_node_base*, uint32_t>& addresses,
                       const std::_Rb_tree_node_base* node, const std::_Rb_tree_node_base* header) {
    if (node == nullptr) {
        return 0;
    }
    if (node == header) {
        return kTreeHeader;
    }
    return addresses.at(node);
}

void mirror(const std::_Rb_tree_node_base* node, const std::_Rb_tree_node_base* header,
            std::map<const std::_Rb_tree_node_base*, uint32_t>& addresses) {
    if (node == nullptr) {
        return;
    }
    addresses[node] = kTreeNodes + static_cast<uint32_t>(addresses.size()) * kNodeStride;
    mirror(node->_M_left, header, addresses);
    mirror(node->_M_right, header, addresses);
}

void write_node(const std::_Rb_tree_node_base* node, const std::_Rb_tree_node_base* header,
                const std::map<const std::_Rb_tree_node_base*, uint32_t>& addresses) {
    if (node == nullptr) {
        return;
    }
    uint32_t address = addresses.at(node);
    bool red = node->_M_color == std::_S_red;
    wp::wr32(address, guest_address(addresses, node->_M_left, header));
    wp::wr32(address + 4, guest_address(addresses, node->_M_right, header));
    wp::wr32(address + 8, guest_address(addresses, node->_M_parent, header) | (red ? kRedBit : 0));
    wp::wr32(address + kKeyOffset, static_cast<uint32_t>(static_cast<const HostNode*>(node)->key));
    write_node(node->_M_left, header, addresses);
    write_node(node->_M_right, header, addresses);
}

int check_subtree(uint32_t node, uint32_t parent, int64_t low, int64_t high, uint32_t& count, bool& ok) {
    if (node == 0) {
        return 1;
    }
    count++;
    uint32_t link = wp::rd32(node + 8);
    int64_t key = static_cast<int32_t>(wp::rd32(node + kKeyOffset));
    if ((link & ~kRedBit) != parent || key <= low || key >= high) {
        ok = false;
        return 0;
    }
    uint32_t left = wp::rd32(node);
    uint32_t right = wp::rd32(node + 4);
    bool red = (link & kRedBit) != 0;
    if (red && ((left && (wp::rd32(left + 8) & kRedBit)) || (right && (wp::rd32(right + 8) & kRedBit)))) {
        ok = false;
        return 0;
    }
    int left_height = check_subtree(left, node, low, key, count, ok);
    int right_height = check_subtree(right, node, key, high, count, ok);
    if (!ok || left_height != right_height) {
        ok = false;
        return 0;
    }
    return left_height + (red ? 0 : 1);
}

bool run_tree_erase(std::mt19937& random, int size) {
    std::set<int> keys;
    while (static_cast<int>(keys.size()) < size) {
        keys.insert(static_cast<int>(random() % 1000));
    }
    std::vector<int> order(keys.begin(), keys.end());
    std::shuffle(order.begin(), order.end(), random);
    std::_Rb_tree_node_base header;
    header._M_color = std::_S_red;
    header._M_parent = nullptr;
    header._M_left = &header;
    header._M_right = &header;
    std::vector<HostNode> nodes(order.size());
    for (size_t i = 0; i < order.size(); i++) {
        nodes[i].key = order[i];
        std::_Rb_tree_node_base* parent = &header;
        std::_Rb_tree_node_base* cursor = header._M_parent;
        bool left = true;
        while (cursor != nullptr) {
            parent = cursor;
            left = order[i] < static_cast<HostNode*>(cursor)->key;
            cursor = left ? cursor->_M_left : cursor->_M_right;
        }
        std::_Rb_tree_insert_and_rebalance(left, &nodes[i], parent, header);
    }
    std::map<const std::_Rb_tree_node_base*, uint32_t> addresses;
    mirror(header._M_parent, &header, addresses);
    write_node(header._M_parent, &header, addresses);
    uint32_t root = guest_address(addresses, header._M_parent, &header);
    wp::wr32(kTreeHeader, root);
    wp::wr32(kTreeHeader + 4, root);
    wp::wr32(kTreeHeader + 8, 0);
    size_t victim_index = random() % nodes.size();
    uint32_t victim = addresses.at(&nodes[victim_index]);
    int victim_key = nodes[victim_index].key;

    wp::Cpu c{};
    c.r[3] = victim;
    c.r[4] = root;
    f_80039510(c);

    uint32_t new_root = wp::rd32(kTreeHeader);
    uint32_t count = 0;
    bool ok = true;
    if (new_root != 0 && (wp::rd32(new_root + 8) & kRedBit)) {
        return false;
    }
    check_subtree(new_root, kTreeHeader, -1, 1000000, count, ok);
    if (!ok || count != nodes.size() - 1) {
        std::fprintf(stderr, "tree erase failure: size %zu erased key %d count %u ok %d\n", nodes.size(), victim_key, count, ok);
        return false;
    }
    return true;
}

uint32_t float_bits(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

float bits_float(uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

bool run_matrix_concat(std::mt19937& random) {
    constexpr uint32_t kA = 0x80300000;
    constexpr uint32_t kB = 0x80300040;
    constexpr uint32_t kOut = 0x80300080;
    std::uniform_real_distribution<float> distribution(-4.0f, 4.0f);
    float a[3][4];
    float b[3][4];
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
            a[row][column] = distribution(random);
            b[row][column] = distribution(random);
            wp::wr32(kA + (row * 4 + column) * 4, float_bits(a[row][column]));
            wp::wr32(kB + (row * 4 + column) * 4, float_bits(b[row][column]));
            wp::wr32(kOut + (row * 4 + column) * 4, 0);
        }
    }
    wp::wr32(0x802f58d0, 0);
    wp::wr32(0x802f58d4, float_bits(1.0f));
    wp::Cpu c{};
    c.r[1] = 0x80280000;
    c.r[3] = kA;
    c.r[4] = kB;
    c.r[5] = kOut;
    f_8014b100(c);
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 4; column++) {
            float expected = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column] + (column == 3 ? a[row][3] : 0.0f);
            float actual = bits_float(wp::rd32(kOut + (row * 4 + column) * 4));
            if (std::fabs(expected - actual) > 1e-3f) {
                std::fprintf(stderr, "matrix concat mismatch at %d,%d: expected %f actual %f\n", row, column, expected, actual);
                return false;
            }
        }
    }
    return true;
}

}

int main() {
    wp::g_memory = static_cast<uint8_t*>(std::calloc(wp::kMemorySize, 1));
    int failures = 0;
    for (int pairs : {0, 1, 3, 10, 11, 25}) {
        if (!run_quote_scan(quoted_text(pairs))) {
            std::fprintf(stderr, "quote scan mismatch with %d pairs\n", pairs);
            failures++;
        }
    }
    if (!run_quote_scan({0x22, 0x22, 0x22, 'q', 0x22, 0x22})) {
        std::fputs("quote scan mismatch on adjacent quotes\n", stderr);
        failures++;
    }
    std::mt19937 random(12345);
    for (int round = 0; round < 3000; round++) {
        if (!run_tree_erase(random, 1 + static_cast<int>(random() % 40))) {
            failures++;
            if (failures > 5) {
                break;
            }
        }
    }
    for (int round = 0; round < 50; round++) {
        if (!run_matrix_concat(random)) {
            failures++;
            break;
        }
    }
    std::free(wp::g_memory);
    if (failures == 0) {
        std::puts("all lifted tests passed");
    }
    return failures == 0 ? 0 : 1;
}
