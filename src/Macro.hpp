#pragma once
// Bot macros in the GDR format.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tc::macro {

// A decoded value: what GDR needs of MessagePack and JSON.
struct Node {
    enum class Kind { Nil, Bool, Int, Float, Str, Array, Map, Other };
    Kind kind = Kind::Nil;
    bool b = false;
    int64_t i = 0;
    double f = 0.0;
    std::string s;
    std::vector<Node> items;
    std::vector<std::pair<std::string, Node>> fields;

    Node const* get(std::string const& key) const {
        for (auto const& [k, v] : fields) {
            if (k == key) return &v;
        }
        return nullptr;
    }
    std::optional<double> number() const {
        if (kind == Kind::Int) return static_cast<double>(i);
        if (kind == Kind::Float) return f;
        return std::nullopt;
    }
    bool truthy() const { return kind == Kind::Bool ? b : (kind == Kind::Int && i != 0); }
};

namespace detail {

class Reader {
public:
    Reader(uint8_t const* data, size_t size) : m_data(data), m_size(size) {}

    // Nothing on malformed or truncated data.
    std::optional<Node> value(int depth = 0) {
        if (depth > 64 || m_pos >= m_size) return std::nullopt;
        uint8_t const tag = m_data[m_pos++];
        Node node;
        if (tag <= 0x7f) return integer(tag);
        if (tag >= 0xe0) return integer(static_cast<int8_t>(tag));
        if ((tag & 0xf0) == 0x80) return map(tag & 0x0f, depth);
        if ((tag & 0xf0) == 0x90) return array(tag & 0x0f, depth);
        if ((tag & 0xe0) == 0xa0) return string(tag & 0x1f);
        switch (tag) {
            case 0xc0: return node;
            case 0xc2: case 0xc3:
                node.kind = Node::Kind::Bool;
                node.b = tag == 0xc3;
                return node;
            case 0xc4: return skipped(uint(1));
            case 0xc5: return skipped(uint(2));
            case 0xc6: return skipped(uint(4));
            case 0xc7: return skipped(uint(1), 1);
            case 0xc8: return skipped(uint(2), 1);
            case 0xc9: return skipped(uint(4), 1);
            case 0xca: {
                auto bits = uint(4);
                if (!bits) return std::nullopt;
                uint32_t const raw = static_cast<uint32_t>(*bits);
                float value;
                std::memcpy(&value, &raw, sizeof value);
                node.kind = Node::Kind::Float;
                node.f = value;
                return node;
            }
            case 0xcb: {
                auto bits = uint(8);
                if (!bits) return std::nullopt;
                double value;
                std::memcpy(&value, &*bits, sizeof value);
                node.kind = Node::Kind::Float;
                node.f = value;
                return node;
            }
            case 0xcc: return unsignedOf(1);
            case 0xcd: return unsignedOf(2);
            case 0xce: return unsignedOf(4);
            case 0xcf: return unsignedOf(8);
            case 0xd0: return signedOf(1);
            case 0xd1: return signedOf(2);
            case 0xd2: return signedOf(4);
            case 0xd3: return signedOf(8);
            case 0xd4: return skipped(size_t{1}, 1);
            case 0xd5: return skipped(size_t{2}, 1);
            case 0xd6: return skipped(size_t{4}, 1);
            case 0xd7: return skipped(size_t{8}, 1);
            case 0xd8: return skipped(size_t{16}, 1);
            case 0xd9: return lengthThen(1, [this](size_t n) { return string(n); });
            case 0xda: return lengthThen(2, [this](size_t n) { return string(n); });
            case 0xdb: return lengthThen(4, [this](size_t n) { return string(n); });
            case 0xdc: return lengthThen(2, [this, depth](size_t n) { return array(n, depth); });
            case 0xdd: return lengthThen(4, [this, depth](size_t n) { return array(n, depth); });
            case 0xde: return lengthThen(2, [this, depth](size_t n) { return map(n, depth); });
            case 0xdf: return lengthThen(4, [this, depth](size_t n) { return map(n, depth); });
            default: return std::nullopt;
        }
    }

private:
    uint8_t const* m_data;
    size_t m_size;
    size_t m_pos = 0;

    // Big-endian unsigned of `bytes` bytes.
    std::optional<uint64_t> uint(size_t bytes) {
        if (m_size - m_pos < bytes) return std::nullopt;
        uint64_t value = 0;
        for (size_t k = 0; k < bytes; ++k) value = (value << 8) | m_data[m_pos++];
        return value;
    }

    static Node integer(int64_t value) {
        Node node;
        node.kind = Node::Kind::Int;
        node.i = value;
        return node;
    }

    std::optional<Node> unsignedOf(size_t bytes) {
        auto value = uint(bytes);
        if (!value) return std::nullopt;
        return integer(static_cast<int64_t>(*value));
    }

    std::optional<Node> signedOf(size_t bytes) {
        auto value = uint(bytes);
        if (!value) return std::nullopt;
        int const shift = static_cast<int>(64 - 8 * bytes);
        return integer(static_cast<int64_t>(*value << shift) >> shift);
    }

    // Binary and extension data: not needed, stepped over.
    std::optional<Node> skipped(std::optional<uint64_t> length, size_t extra = 0) {
        if (!length || m_size - m_pos < *length + extra) return std::nullopt;
        m_pos += static_cast<size_t>(*length) + extra;
        Node node;
        node.kind = Node::Kind::Other;
        return node;
    }

    template <class Then>
    std::optional<Node> lengthThen(size_t bytes, Then then) {
        auto length = uint(bytes);
        if (!length) return std::nullopt;
        return then(static_cast<size_t>(*length));
    }

    std::optional<Node> string(size_t length) {
        if (m_size - m_pos < length) return std::nullopt;
        Node node;
        node.kind = Node::Kind::Str;
        node.s.assign(reinterpret_cast<char const*>(m_data + m_pos), length);
        m_pos += length;
        return node;
    }

    std::optional<Node> array(size_t count, int depth) {
        if (count > m_size - m_pos) return std::nullopt;   // every element takes a byte at least
        Node node;
        node.kind = Node::Kind::Array;
        node.items.reserve(count);
        for (size_t k = 0; k < count; ++k) {
            auto item = value(depth + 1);
            if (!item) return std::nullopt;
            node.items.push_back(std::move(*item));
        }
        return node;
    }

    std::optional<Node> map(size_t count, int depth) {
        if (count > m_size - m_pos) return std::nullopt;
        Node node;
        node.kind = Node::Kind::Map;
        node.fields.reserve(count);
        for (size_t k = 0; k < count; ++k) {
            auto key = value(depth + 1);
            auto item = key ? value(depth + 1) : std::nullopt;
            if (!item) return std::nullopt;
            node.fields.emplace_back(key->kind == Node::Kind::Str ? key->s : std::string(), std::move(*item));
        }
        return node;
    }
};

} // namespace detail

inline std::optional<Node> decodeMsgpack(std::vector<uint8_t> const& bytes) {
    return detail::Reader(bytes.data(), bytes.size()).value();
}

// One input of a macro: a press or release of `button` on physics frame `frame`.
struct Input {
    int frame = 0;
    int button = 1;
    bool player2 = false;
    bool down = false;
};

// xdBot's "Frame Fixes": where the players were on a frame.
struct FrameFix {
    int frame = 0;
    float x1 = 0.f, y1 = 0.f;
    bool hasP2 = false;
    float x2 = 0.f, y2 = 0.f;
};

struct Macro {
    double framerate = 240.0;
    int levelID = 0;
    std::string levelName;
    std::string bot;
    std::vector<Input> inputs;
    std::vector<FrameFix> fixes;
};

// The macro in a decoded GDR document, or an error.
inline std::pair<std::optional<Macro>, std::string> macroFromNode(Node const& root) {
    if (root.kind != Node::Kind::Map) return {std::nullopt, "not a GDR macro"};
    auto const* inputs = root.get("inputs");
    if (!inputs || inputs->kind != Node::Kind::Array) return {std::nullopt, "the macro has no inputs"};

    Macro macro;
    if (auto const* rate = root.get("framerate"); rate && rate->number()) macro.framerate = *rate->number();
    if (auto const* level = root.get("level"); level && level->kind == Node::Kind::Map) {
        if (auto const* id = level->get("id"); id && id->number()) macro.levelID = static_cast<int>(*id->number());
        if (auto const* name = level->get("name"); name && name->kind == Node::Kind::Str) macro.levelName = name->s;
    }
    if (auto const* bot = root.get("bot"); bot && bot->kind == Node::Kind::Map) {
        if (auto const* name = bot->get("name"); name && name->kind == Node::Kind::Str) macro.bot = name->s;
    }
    if (auto const* fixes = root.get("frameFixes"); fixes && fixes->kind == Node::Kind::Array) {
        auto point = [](Node const* player, float& x, float& y) {
            if (!player || player->kind != Node::Kind::Map) return false;
            auto const* px = player->get("x");
            auto const* py = player->get("y");
            if (!px || !py || !px->number() || !py->number()) return false;
            x = static_cast<float>(*px->number());
            y = static_cast<float>(*py->number());
            return true;
        };
        for (auto const& item : fixes->items) {
            auto const* frame = item.get("frame");
            FrameFix fix;
            if (!frame || !frame->number() || !point(item.get("p1"), fix.x1, fix.y1)) continue;
            fix.frame = static_cast<int>(*frame->number());
            fix.hasP2 = point(item.get("p2"), fix.x2, fix.y2);
            macro.fixes.push_back(fix);
        }
    }

    for (auto const& item : inputs->items) {
        auto const* frame = item.get("frame");
        if (!frame || !frame->number()) return {std::nullopt, "an input of the macro has no frame"};
        Input input;
        input.frame = static_cast<int>(*frame->number());
        if (auto const* button = item.get("btn"); button && button->number()) input.button = static_cast<int>(*button->number());
        if (auto const* p2 = item.get("2p")) input.player2 = p2->truthy();
        if (auto const* down = item.get("down")) input.down = down->truthy();
        if (input.button < 1 || input.button > 3) continue;
        macro.inputs.push_back(input);
    }
    if (macro.inputs.empty()) return {std::nullopt, "the macro has no inputs"};
    if (!(macro.framerate > 0.0)) return {std::nullopt, "the macro has no framerate"};
    return {std::move(macro), {}};
}

// GDR2: "GDR", varint version, then metadata and the inputs of player 1 followed by those of player 2.
inline std::pair<std::optional<Macro>, std::string> readGdr2(std::vector<uint8_t> const& bytes) {
    size_t pos = 0;
    bool ok = true;
    auto varint = [&]() -> uint64_t {
        uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (pos >= bytes.size()) break;
            uint8_t const b = bytes[pos++];
            value |= static_cast<uint64_t>(b & 0x7f) << shift;
            if (!(b & 0x80)) return value;
        }
        ok = false;
        return 0;
    };
    auto raw = [&](size_t count) -> uint8_t const* {
        if (bytes.size() - pos < count) {
            ok = false;
            pos = bytes.size();
            return nullptr;
        }
        auto const* at = bytes.data() + pos;
        pos += count;
        return at;
    };
    auto text = [&]() {
        uint64_t const length = varint();
        auto const* at = raw(static_cast<size_t>(std::min<uint64_t>(length, bytes.size())));
        return at ? std::string(reinterpret_cast<char const*>(at), static_cast<size_t>(length)) : std::string();
    };
    auto bigEndian = [&](size_t count) -> uint64_t {
        auto const* at = raw(count);
        uint64_t value = 0;
        for (size_t k = 0; at && k < count; ++k) value = (value << 8) | at[k];
        return value;
    };

    auto const* magic = raw(3);
    if (!magic || std::memcmp(magic, "GDR", 3) != 0) return {std::nullopt, "not a GDR2 macro"};
    if (varint() != 2) return {std::nullopt, "unknown GDR2 version"};
    bool const inputExtension = !text().empty();
    Macro macro;
    text();                         // author
    text();                         // description
    bigEndian(4);                   // duration (seconds, float)
    varint();                       // game version
    uint64_t const rate = bigEndian(8);
    std::memcpy(&macro.framerate, &rate, sizeof rate);
    // Some Eclipse versions wrote the display FPS here while the frames count physics steps at 240.
    if (!(macro.framerate >= 120.0)) macro.framerate = 240.0;
    varint();                       // seed
    varint();                       // coins
    raw(1);                         // low detail mode
    auto const* platformer = raw(1);
    macro.bot = text();
    varint();                       // bot version
    macro.levelID = static_cast<int>(varint());
    macro.levelName = text();
    raw(static_cast<size_t>(varint()));            // replay extension
    for (uint64_t deaths = varint(); ok && deaths > 0; --deaths) varint();
    varint();                       // input count of both players
    uint64_t const firstPlayer = varint();
    if (!ok) return {std::nullopt, "the GDR2 header is broken"};

    bool const isPlatformer = platformer && *platformer;
    int64_t frame[2] = {0, 0};
    for (uint64_t index = 0; pos < bytes.size(); ++index) {
        uint64_t const packed = varint();
        if (inputExtension) raw(static_cast<size_t>(varint()));
        if (!ok) return {std::nullopt, "the inputs of the GDR2 macro are broken"};
        bool const second = index >= firstPlayer;
        Input input;
        input.player2 = second;
        input.down = packed & 1;
        input.button = isPlatformer ? static_cast<int>((packed >> 1) & 3) : 1;
        frame[second] += static_cast<int64_t>(packed >> (isPlatformer ? 3 : 1));
        input.frame = static_cast<int>(frame[second]);
        if (input.button >= 1) macro.inputs.push_back(input);
    }
    std::stable_sort(macro.inputs.begin(), macro.inputs.end(),
        [](Input const& a, Input const& b) { return a.frame < b.frame; });
    if (macro.inputs.empty()) return {std::nullopt, "the macro has no inputs"};
    return {std::move(macro), {}};
}

} // namespace tc::macro
