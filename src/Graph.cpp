// The graph of a counted run: windows, precision and their spread over the level.

#include "App.hpp"

namespace tc {

namespace {

enum class GraphMode { Windows, Precision, Spread };

// One shown click of the run.
struct GraphPoint {
    size_t index = 0;
    double percent = 0.0;     // of the level, at the click
    double seconds = 0.0;     // from the start of the run
    double frames = 0.0;      // shown window
    double precision = 0.0;   // share of the run's L* reached with this click, percent
    bool precise = false;
    std::string marks;        // symbols after the window
};

// What the graph draws: the current results of the level or a saved run.
struct GraphData {
    std::vector<GraphPoint> points;
    bool percentKnown = false;   // the level length is known: X can be the level percent
    bool lstarKnown = false;
    double lstarTotal = 0.0;
    bool hudPrecise = false;     // bucket scheme of the counter
    bool trainable = false;      // the current results in a level: a click can be trained
};

// `lstar` per input (NaN for hidden ones), nullptr while it is not known.
GraphData graphData(RunView const& run, float levelLength, std::vector<double> const* lstar, double lstarTotal,
    bool hudPrecise) {
    GraphData data;
    data.percentKnown = levelLength > 0.f;
    data.lstarKnown = lstar && lstarTotal > 0.0;
    data.lstarTotal = lstarTotal;
    data.hudPrecise = hudPrecise;
    double const rawTps = static_cast<double>(cfg::rawTps());
    for (size_t i = 0; i < run.events.size() && i < run.windows.size(); ++i) {
        auto const& event = run.events[i];
        if (!overlay::isShown(event, run.windows[i])) continue;
        GraphPoint point;
        point.index = i;
        if (data.percentKnown) point.percent = std::clamp(100.0 * event.x / levelLength, 0.0, 100.0);
        point.seconds = event.tick / rawTps;
        point.frames = shownWindow(run, i);
        point.precise = cfg::isPrecise(run.windows[i]);
        point.marks = resultMarks(run, i);
        if (data.lstarKnown && i < lstar->size() && !std::isnan((*lstar)[i]))
            point.precision = 100.0 * (*lstar)[i] / lstarTotal;
        data.points.push_back(point);
    }
    return data;
}

// A step for axis marks: 1, 2 or 5 times a power of ten, about `count` marks.
double niceStep(double range, int count) {
    double const raw = range / std::max(1, count);
    double const power = std::pow(10.0, std::floor(std::log10(raw)));
    for (double factor : {1.0, 2.0, 5.0}) {
        if (factor * power >= raw) return factor * power;
    }
    return 10.0 * power;
}

ccColor4F color4(ccColor3B color, float alpha = 1.f) {
    return {color.r / 255.f, color.g / 255.f, color.b / 255.f, alpha};
}

class GraphPopup : public geode::Popup {
    static constexpr float kWidth = 440.f;
    static constexpr float kHeight = 280.f;
    // The plot area inside the popup.
    static constexpr float kLeft = 52.f;
    static constexpr float kRight = kWidth - 22.f;
    static constexpr float kBottom = 58.f;
    static constexpr float kTop = kHeight - 66.f;

    GraphMode m_mode = GraphMode::Windows;
    bool m_byTime = false;
    GraphData m_data;
    CCNode* m_plot = nullptr;
    CCMenu* m_buttons = nullptr;
    CCLabelBMFont* m_info = nullptr;
    CCDrawNode* m_mark = nullptr;
    double m_maxX = 100.0;
    double m_maxY = 1.0;
    std::optional<size_t> m_selected;   // input index of the tapped click

public:
    static GraphPopup* create(GraphData data) {
        auto* popup = new GraphPopup();
        if (popup->setup(std::move(data))) {
            popup->autorelease();
            return popup;
        }
        delete popup;
        return nullptr;
    }

protected:
    bool setup(GraphData data) {
        if (!Popup::init(kWidth, kHeight)) return false;
        this->setTitle("Run Graph");
        m_data = std::move(data);
        // Without the level length (runs saved by older versions) only time works.
        m_byTime = !m_data.percentKnown;
        m_info = CCLabelBMFont::create("", "chatFont.fnt");
        m_info->setScale(0.7f);
        m_info->setPosition({kWidth / 2.f, 18.f});
        m_mainLayer->addChild(m_info);
        this->refresh();
        return true;
    }

    void button(char const* text, bool on, SEL_MenuHandler selector) {
        auto* sprite = ButtonSprite::create(text, "goldFont.fnt", on ? "GJ_button_01.png" : "GJ_button_04.png", 0.6f);
        sprite->setScale(0.6f);
        m_buttons->addChild(CCMenuItemSpriteExtra::create(sprite, this, selector));
    }

    void label(std::string const& text, CCPoint at, CCPoint anchor, float scale = 0.45f) {
        auto* node = CCLabelBMFont::create(text.c_str(), "chatFont.fnt");
        node->setScale(scale);
        node->setAnchorPoint(anchor);
        node->setPosition(at);
        node->setOpacity(200);
        m_plot->addChild(node);
    }

    double xOf(GraphPoint const& point) const { return m_byTime ? point.seconds : point.percent; }
    double yOf(GraphPoint const& point) const { return m_mode == GraphMode::Precision ? point.precision : point.frames; }

    CCPoint toPlot(double x, double y) const {
        return {static_cast<float>(kLeft + (kRight - kLeft) * x / m_maxX),
            static_cast<float>(kBottom + (kTop - kBottom) * std::clamp(y / m_maxY, 0.0, 1.0))};
    }

    void refresh() {
        if (m_buttons) m_buttons->removeFromParent();
        m_buttons = CCMenu::create();
        m_buttons->setContentSize({kWidth - 40.f, 26.f});
        m_buttons->ignoreAnchorPointForPosition(false);
        m_buttons->setPosition({kWidth / 2.f, kHeight - 46.f});
        m_buttons->setLayout(RowLayout::create()->setGap(6.f)->setAutoScale(true));
        button("Windows", m_mode == GraphMode::Windows, menu_selector(GraphPopup::onWindows));
        button("Precision", m_mode == GraphMode::Precision, menu_selector(GraphPopup::onPrecision));
        button("Spread", m_mode == GraphMode::Spread, menu_selector(GraphPopup::onSpread));
        button(m_byTime ? "X: Time" : "X: Level %", m_data.percentKnown, menu_selector(GraphPopup::onAxis));
        if (m_data.trainable) button("Train", m_mode != GraphMode::Spread, menu_selector(GraphPopup::onTrain));
        m_buttons->updateLayout();
        m_mainLayer->addChild(m_buttons);

        if (m_plot) m_plot->removeFromParent();
        m_plot = CCNode::create();
        m_mainLayer->addChild(m_plot);
        m_mark = nullptr;
        if (m_data.points.empty()) {
            m_info->setString("No counted clicks to draw.");
            return;
        }
        if (m_mode == GraphMode::Spread) drawSpread();
        else drawOverLevel();
    }

    void drawAxes(CCDrawNode* draw, double yStep, std::string const& yUnit) {
        ccColor4F const axis{1.f, 1.f, 1.f, 0.5f};
        ccColor4F const grid{1.f, 1.f, 1.f, 0.08f};
        draw->drawSegment({kLeft, kBottom}, {kRight, kBottom}, 0.6f, axis);
        draw->drawSegment({kLeft, kBottom}, {kLeft, kTop}, 0.6f, axis);
        for (double y = yStep; y <= m_maxY + 1e-9; y += yStep) {
            auto const at = toPlot(0.0, y);
            draw->drawSegment(at, {kRight, at.y}, 0.4f, grid);
            label(fmt::format("{:g}{}", y, yUnit), {kLeft - 4.f, at.y}, {1.f, 0.5f});
        }
        double const xStep = niceStep(m_maxX, 5);
        for (double x = 0.0; x <= m_maxX + 1e-9; x += xStep) {
            auto const at = toPlot(x, 0.0);
            draw->drawSegment(at, {at.x, at.y - 3.f}, 0.5f, axis);
            label(m_byTime ? fmt::format("{:g}s", x) : fmt::format("{:g}%", x), {at.x, kBottom - 5.f}, {0.5f, 1.f});
        }
    }

    // Windows or precision against the level.
    void drawOverLevel() {
        m_maxX = 100.0;
        if (m_byTime) {
            m_maxX = 1.0;
            for (auto const& point : m_data.points) m_maxX = std::max(m_maxX, point.seconds);
        }
        bool const precision = m_mode == GraphMode::Precision;
        if (precision && !m_data.lstarKnown) {
            m_info->setString("Precision (L*) is not known for this run yet.");
            return;
        }
        m_maxY = 100.0;
        if (!precision) {
            m_maxY = 1.0;
            for (auto const& point : m_data.points) m_maxY = std::max(m_maxY, point.frames);
            m_maxY = std::ceil(m_maxY);
        }

        auto* draw = CCDrawNode::create();
        m_plot->addChild(draw);
        drawAxes(draw, precision ? 25.0 : niceStep(m_maxY, 4), precision ? "%" : "");
        CCPoint previous{kLeft, kBottom};
        for (auto const& point : m_data.points) {
            auto const at = toPlot(xOf(point), yOf(point));
            if (precision) {
                draw->drawSegment(previous, at, 0.9f, {0.45f, 0.85f, 1.f, 1.f});
                previous = at;
            } else {
                auto const color = overlay::windowColor(point.frames, m_data.hudPrecise);
                draw->drawSegment({at.x, kBottom}, at, 0.5f, color4(color, 0.25f));
                draw->drawDot(at, 1.8f, color4(color));
            }
        }

        if (precision) {
            // Where half and nine tenths of the run's precision are reached.
            auto reached = [&](double share) {
                for (auto const& point : m_data.points) {
                    if (point.precision >= share) return xOf(point);
                }
                return xOf(m_data.points.back());
            };
            auto const unit = m_byTime ? "s" : "%";
            m_info->setString(fmt::format("L* {:.2f} = 100%. Half of it by {:.1f}{}, 90% by {:.1f}{}. Tap the graph.",
                m_data.lstarTotal, reached(50.0), unit, reached(90.0), unit).c_str());
        } else {
            auto const hardest = std::min_element(m_data.points.begin(), m_data.points.end(),
                [](GraphPoint const& a, GraphPoint const& b) { return a.frames < b.frames; });
            m_info->setString(fmt::format("{} clicks, hardest {} at {:.1f}%. Tap the graph.", m_data.points.size(),
                cfg::windowLabel(hardest->frames, hardest->precise, false), hardest->percent).c_str());
        }
    }

    // How many clicks fall into each counter bucket.
    void drawSpread() {
        size_t const buckets = bucketCount(m_data.hudPrecise);
        std::array<int, kBucketCount> counts{};
        for (auto const& point : m_data.points) ++counts[bucketOf(point.frames)];
        int const most = *std::max_element(counts.begin(), counts.end());
        m_maxX = static_cast<double>(buckets);
        m_maxY = std::max(1, most);

        auto* draw = CCDrawNode::create();
        m_plot->addChild(draw);
        draw->drawSegment({kLeft, kBottom}, {kRight, kBottom}, 0.6f, {1.f, 1.f, 1.f, 0.5f});
        float const slot = (kRight - kLeft) / static_cast<float>(buckets);
        for (size_t i = 0; i < buckets; ++i) {
            auto const color = overlay::bucketColor(i, m_data.hudPrecise);
            float const x0 = kLeft + slot * static_cast<float>(i) + 4.f;
            float const x1 = x0 + slot - 8.f;
            float const top = toPlot(0.0, counts[i]).y;
            if (counts[i] > 0) {
                CCPoint corners[4] = {{x0, kBottom}, {x1, kBottom}, {x1, top}, {x0, top}};
                draw->drawPolygon(corners, 4, color4(color, 0.8f), 0.f, color4(color));
            }
            label(std::to_string(counts[i]), {(x0 + x1) / 2.f, top + 2.f}, {0.5f, 0.f}, 0.5f);
            auto const [lo, hi] = bucketBounds(i, m_data.hudPrecise, cfg::maxDisplayFrames());
            auto const name = lo == hi ? fmt::format("{:g}", lo) : fmt::format("{:g}-{:g}", lo, hi);
            label(name, {(x0 + x1) / 2.f, kBottom - 5.f}, {0.5f, 1.f}, 0.4f);
        }
        m_info->setString(fmt::format("{} clicks by window (frames), like the counter.", m_data.points.size()).c_str());
    }

    // Tapping the plot shows the click under the finger.
    bool ccTouchBegan(CCTouch* touch, CCEvent* event) override {
        bool const result = Popup::ccTouchBegan(touch, event);
        if (m_mode == GraphMode::Spread || m_data.points.empty() || !m_plot) return result;
        auto const at = m_mainLayer->convertToNodeSpace(touch->getLocation());
        if (at.x < kLeft - 6.f || at.x > kRight + 6.f || at.y < kBottom - 6.f || at.y > kTop + 6.f) return result;
        double const x = (at.x - kLeft) / (kRight - kLeft) * m_maxX;
        auto const nearest = std::min_element(m_data.points.begin(), m_data.points.end(),
            [&](GraphPoint const& a, GraphPoint const& b) { return std::abs(xOf(a) - x) < std::abs(xOf(b) - x); });
        m_selected = nearest->index;
        if (m_mark) m_mark->removeFromParent();
        m_mark = CCDrawNode::create();
        auto const dot = toPlot(xOf(*nearest), yOf(*nearest));
        m_mark->drawSegment({dot.x, kBottom}, {dot.x, kTop}, 0.5f, {1.f, 1.f, 1.f, 0.35f});
        m_mark->drawDot(dot, 3.f, {1.f, 1.f, 1.f, 1.f});
        m_plot->addChild(m_mark);
        auto const where = m_data.percentKnown ? fmt::format("{:.1f}% ({:.2f}s)", nearest->percent, nearest->seconds)
                                               : fmt::format("{:.2f}s", nearest->seconds);
        auto const precision = m_data.lstarKnown ? fmt::format(", precision {:.1f}%", nearest->precision) : "";
        m_info->setString(fmt::format("Click #{} at {}: {}{}{}", nearest->index + 1, where,
            cfg::windowLabel(nearest->frames, nearest->precise, false), nearest->marks, precision).c_str());
        return result;
    }

    void onWindows(CCObject*) { m_mode = GraphMode::Windows; this->refresh(); }
    void onPrecision(CCObject*) { m_mode = GraphMode::Precision; this->refresh(); }
    void onSpread(CCObject*) { m_mode = GraphMode::Spread; this->refresh(); }
    void onAxis(CCObject*) {
        if (!m_data.percentKnown) return;
        m_byTime = !m_byTime;
        this->refresh();
    }

    size_t bucketOf(double frames) const {
        return static_cast<size_t>(bucketIndex(frames, m_data.hudPrecise));
    }

    // Trains the tapped click: every popup closes and the bot plays up to it.
    void onTrain(CCObject*) {
        if (!m_selected) {
            m_info->setString("Tap a click on the graph first.");
            return;
        }
        auto const error = startTraining(*m_selected);
        if (!error.empty()) {
            m_info->setString(error.c_str());
            return;
        }
        std::vector<FLAlertLayer*> others;
        if (auto* scene = CCDirector::sharedDirector()->getRunningScene()) {
            for (auto* node : CCArrayExt<CCNode*>(scene->getChildren())) {
                if (auto* popup = typeinfo_cast<FLAlertLayer*>(node); popup && popup != this) others.push_back(popup);
            }
        }
        for (auto* popup : others) popup->keyBackClicked();
        this->onClose(nullptr);
        resumeFromPause();
    }
};

} // namespace

// The graph of the current results (TC menu in a level).
void openGraph() {
    auto const& s = Session::get();
    auto* play = PlayLayer::get();
    auto data = graphData(s, play ? play->m_levelLength : 0.f, s.lstarReady ? &s.lstar : nullptr, s.lstarTotal,
        s.hudPrecise);
    data.trainable = play != nullptr;
    if (auto* popup = GraphPopup::create(std::move(data))) popup->show();
}

// The graph of a saved run (library).
void openGraph(RunView const& run, float levelLength, std::vector<double> const* lstar, double lstarTotal,
    bool precise) {
    if (auto* popup = GraphPopup::create(graphData(run, levelLength, lstar, lstarTotal, precise))) popup->show();
}

} // namespace tc
