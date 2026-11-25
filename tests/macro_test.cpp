// Tests of the GDR macro reader (MessagePack form).
#include "../src/Macro.hpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

using namespace tc::macro;

namespace {

std::vector<uint8_t> bytes(std::initializer_list<int> list) {
    std::vector<uint8_t> out;
    for (int b : list) out.push_back(static_cast<uint8_t>(b));
    return out;
}

void str(std::vector<uint8_t>& out, char const* text) {
    size_t const n = std::strlen(text);
    out.push_back(static_cast<uint8_t>(0xa0 | n));
    out.insert(out.end(), text, text + n);
}

// {"framerate": 240.0, "level": {"id": 5, "name": "x"}, "bot": {"name": "xdBot"},
//  "frameFixes": [1, 2], "inputs": [{"frame": 300, "btn": 1, "2p": false, "down": true},
//  {"frame": -1, "btn": 9, "down": false}, {"frame": 70000, "btn": 2, "2p": true, "down": false}]}
std::vector<uint8_t> sample() {
    std::vector<uint8_t> out = bytes({0x85});
    str(out, "framerate");
    for (int b : {0xcb, 0x40, 0x6e, 0, 0, 0, 0, 0, 0}) out.push_back(static_cast<uint8_t>(b));
    str(out, "level");
    out.push_back(0x82);
    str(out, "id");
    out.push_back(5);
    str(out, "name");
    str(out, "x");
    str(out, "bot");
    out.push_back(0x81);
    str(out, "name");
    str(out, "xdBot");
    str(out, "frameFixes");
    for (int b : {0x92, 1, 2}) out.push_back(static_cast<uint8_t>(b));
    str(out, "inputs");
    out.push_back(0x93);
    out.push_back(0x84);
    str(out, "frame");
    for (int b : {0xcd, 0x01, 0x2c}) out.push_back(static_cast<uint8_t>(b));
    str(out, "btn");
    out.push_back(1);
    str(out, "2p");
    out.push_back(0xc2);
    str(out, "down");
    out.push_back(0xc3);
    out.push_back(0x83);
    str(out, "frame");
    out.push_back(0xff);
    str(out, "btn");
    out.push_back(9);
    str(out, "down");
    out.push_back(0xc2);
    out.push_back(0x84);
    str(out, "frame");
    for (int b : {0xce, 0, 0x01, 0x11, 0x70}) out.push_back(static_cast<uint8_t>(b));
    str(out, "btn");
    out.push_back(2);
    str(out, "2p");
    out.push_back(0xc3);
    str(out, "down");
    out.push_back(0xc2);
    return out;
}

} // namespace

int main() {
    auto const node = decodeMsgpack(sample());
    assert(node);
    auto const [macro, error] = macroFromNode(*node);
    assert(macro && error.empty());
    assert(macro->framerate == 240.0);
    assert(macro->levelID == 5 && macro->levelName == "x");
    assert(macro->bot == "xdBot");
    // Fixes without positions are left out.
    assert(macro->fixes.empty());
    // The input with button 9 is not a GD button and is left out.
    assert(macro->inputs.size() == 2);
    assert(macro->inputs[0].frame == 300 && macro->inputs[0].button == 1 && !macro->inputs[0].player2 &&
        macro->inputs[0].down);
    assert(macro->inputs[1].frame == 70000 && macro->inputs[1].button == 2 && macro->inputs[1].player2 &&
        !macro->inputs[1].down);

    // Negative fixint and int16.
    assert(decodeMsgpack(bytes({0xff}))->i == -1);
    assert(decodeMsgpack(bytes({0xd1, 0xff, 0x38}))->i == -200);

    // Truncated data is rejected, not read past its end.
    auto cut = sample();
    cut.resize(cut.size() - 3);
    assert(!decodeMsgpack(cut));
    assert(!decodeMsgpack(bytes({0xdd, 0xff, 0xff, 0xff, 0xff})));

    // Not a macro.
    assert(!macroFromNode(*decodeMsgpack(bytes({0x05}))).first);
    assert(!macroFromNode(*decodeMsgpack(bytes({0x80}))).first);

    // GDR2: header, then P1 inputs (delta << 1 | down), then P2.
    {
        std::vector<uint8_t> g = bytes({'G', 'D', 'R', 2, 0, 1, 'a', 0, 0x43, 0, 0, 0, 0,
            0x40, 0x4d, 0x80, 0, 0, 0, 0, 0,      // framerate 59: taken as 240
            0, 0, 0, 0, 3, 'b', 'o', 't', 1, 5, 1, 'x', 0, 0,
            3, 2,                                  // 3 inputs, 2 of player 1
            0x83, 0x05,                            // P1: frame 321, press
            0x58,                                  // P1: frame 365, release
            0x81, 0x01});                          // P2: frame 64, press
        auto const [m, error] = readGdr2(g);
        assert(m && error.empty());
        assert(m->framerate == 240.0 && m->levelID == 5 && m->levelName == "x" && m->bot == "bot");
        assert(m->inputs.size() == 3);
        assert(m->inputs[0].frame == 64 && m->inputs[0].player2 && m->inputs[0].down);
        assert(m->inputs[1].frame == 321 && !m->inputs[1].player2 && m->inputs[1].down);
        assert(m->inputs[2].frame == 365 && !m->inputs[2].down);
        g.pop_back();
        assert(!readGdr2(g).first);
    }

    std::puts("macro tests passed");
}
