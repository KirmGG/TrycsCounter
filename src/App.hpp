#pragma once
// Declarations shared by the source files of the mod (namespace tc).

#include <Geode/Geode.hpp>
#include "ModSettings.hpp"
#include "Overlay.hpp"
#include "Precision.hpp"
#include "SavedRuns.hpp"
#include "ProbePlanner.hpp"
#include "Session.hpp"
#include "WindowMath.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace tc {

inline constexpr double kCbfSmallestFactor = std::numeric_limits<float>::min();

// An input may be injected this many raw ticks after its recorded tick.
inline constexpr int kInjectLateToleranceTicks = 4;

// A trial whose players are back in exactly the baseline state for this long after every moved input replays
// the rest of the baseline.
inline constexpr int kConvergeHoldTicks = 24 * cfg::kRawTicksPerPhysicsFrame;

enum class TraceCheck {
    Recorded,     // replay against the recorded attempt: positions only, loose
    Checkpoint,   // trial against the baseline replay: everything, tight
    Exact,        // convergence: bit for bit
    Near          // convergence within the "Back on path" tolerance
};

// The results of a counted run as the display reads them.
struct RunView {
    std::vector<InputEvent> const& events;
    std::vector<WindowResult> const& windows;
    std::vector<DeepRange> const& deep;
    std::vector<char> const& dependent;
    std::vector<char> const& uncertain;
    std::vector<int> const& swift;
    std::vector<char> const& otherMarker;
    int mismatchTick = -1;

    RunView(Session const& s)
        : events(s.source.events), windows(s.windows), deep(s.deep), dependent(s.dependent),
          uncertain(s.edgeUncertain), swift(s.sourceSwift), otherMarker(s.otherMarker),
          mismatchTick(s.resultsMismatchTick) {}
    RunView(saved::SavedRun const& run, std::vector<int> const& swiftLevels)
        : events(run.run.events), windows(run.windows), deep(run.deep), dependent(run.dependent),
          uncertain(run.uncertain), swift(swiftLevels), otherMarker(run.otherMarker), mismatchTick(run.mismatchTick) {}
};

// Common.cpp
int currentTick(GJBaseGameLayer* layer);
void alignTickToCheckpoint(PlayLayer* play, CheckpointObject* checkpoint);
void alignTickAfterReset(PlayLayer* play);
void pruneCheckpointTicks(PlayLayer* play);
void saveCheckpointExtra(PlayLayer* play, CheckpointObject* checkpoint);
void applyCheckpointExtra(PlayLayer* play);
int currentLevelID(PlayLayer* layer);
float startPosX(PlayLayer* play);
std::string startMismatch(PlayLayer* play, float startX);
CCPoint playerPoint(GJBaseGameLayer* layer, bool player1);
void placeEvent(GJBaseGameLayer* layer, InputEvent& event);
InputEvent markerEvent(RunView const& run, size_t index);
int injectTick(Session const& s, InputEvent const& event);
size_t partnerOf(std::vector<InputEvent> const& events, size_t index);
bool moveHold();
bool adaptClicks();
int horizonTicks(Session const& s, size_t index);
std::vector<PlannerInput> plannerInputs(std::vector<InputEvent> const& events, int units,
    std::vector<char> const* freeFlight = nullptr);
PlannerConfig plannerConfig(Session const& s, int endTick, RunEnd endKind);
bool recordingWanted();
bool liveMode();
bool trajectoryLive();
Stage idleStage(PlayLayer* play);
void sortByTick(std::vector<InputEvent>& events);
void forceClickOnSteps(GJBaseGameLayer* layer);
void muteAudio(bool mute);
void freezePlayerStats(PlayLayer* play, bool testRun);
void restorePlayerStats(PlayLayer* play);

// Cbf.cpp
Mod* cbfMod();
bool cbfSplitsSteps();
void finalizeCbfStep(Session& s);
void noteStepDt(Session& s);
float replayStepDt(Session const& s, int tick, bool player1, float dt);
void rollCbfStep(Session& s, int tick);
int eventPos(InputEvent const& event, int units);
InputEvent shiftedEvent(InputEvent event, int shift, int units);
bool cbfCanSplit(PlayerObject* player);
void resetCollisionLog(PlayerObject* player);
void configureUnits(Session& s, PlayLayer* play, std::vector<InputEvent> const& events);
bool replayCbfStep(PlayLayer* play, PlayerObject* p1, float dt, int tick);

// Recording.cpp
void recordInput(GJBaseGameLayer* layer, bool down, int button, bool player1);
RunRecord recordedRun(PlayLayer* play, int endTick, RunEnd kind);
RunRecord selectRun(PlayLayer* play);

// Analysis.cpp
void startProbing(Session& s);
bool trimArrayTo(PlayLayer* play, size_t count, CCObject* owned = nullptr);
int frameUnits(Session const& s);
bool keepsButtonOrder(std::vector<InputEvent> const& original, std::vector<InputEvent> moved);
void advanceProbing(PlayLayer* play, Session& s);
void beginSegment(PlayLayer* play, Session& s);
void warnAboutCbf();
std::string startAnalysis(PlayLayer* play, RunRecord run, bool deep = false);
void queuePendingAnalysis();
bool startReplay(PlayLayer* play);

// DeepCount.cpp
void buildDeepJobs(Session& s);
bool beginNextDeepJob(Session& s);
void finishDeep(Session& s);

// EdgeCheck.cpp
bool prepareEdgeChecks(Session& s);
// Hitborders.cpp
bool prepareBorderJobs(Session& s);
void recordBorderBaseline(PlayLayer* play, Session& s, int tick);
void recordBorderTrial(PlayLayer* play, Session& s, int tick);
void keepBorderTrial(Session& s, bool survived);
void clearHitborders(PlayLayer* play);
void playHitborders(PlayLayer* play);
void startPendingBorderReplay(PlayLayer* play);
void drawHitborders(PlayLayer* play);
void updateHitborders(PlayLayer* play);
void finishEdgePhase(PlayLayer* play, Session& s);
void applyEdgeChecks(Session& s);

// Trace.cpp
PlayerTrace playerTrace(PlayerObject* player);
TraceState sampleTrace(PlayLayer* play);
float pathTolerance();
bool sameTrace(TraceState const& a, TraceState const& b, TraceCheck check);
TraceState const* traceAt(std::vector<TraceState> const& trace, int tick);
void tracePlayer(PlayLayer* play, std::vector<TraceState>& trace, int tick);
void traceBaseline(PlayLayer* play, Session& s, int tick);
std::vector<char> freeFlightInputs(Session const& s);

// ReplayFixes.cpp
void applyStateFixes(PlayLayer* play, Session& s, int tick);
void syncHeldButtons(PlayLayer* play, Session& s, int tick);
void rejectCheckpoint(Session& s, int tick, char const* what);
void verifyTrialPrefix(PlayLayer* play, Session& s, int tick);
void checkConvergence(PlayLayer* play, Session& s, int tick);

// LayoutMode.cpp
bool layoutBlocksTrigger(EffectGameObject* trigger);
bool layoutSkipsTrigger(int id);
void hideDecoration(PlayLayer* play);
bool layoutFallback(Session& s);
void endAnalysisEnvironment(PlayLayer* play);

// Results.cpp
bool resultsForLevel(PlayLayer* play);
bool saveCurrentRun(PlayLayer* play, std::string const& name = {});
bool loadSavedRun(PlayLayer* play, std::filesystem::path const& path, bool quiet = false);
size_t savedRunCount(std::string const& key);
void saveLastCount(PlayLayer* play);
void loadLevelResults(PlayLayer* play);

// Macros.cpp
void pickMacro();
bool queuedCountFinished(PlayLayer* play, bool counted, std::string const& reason);

// Graph.cpp
void openGraph();
void openGraph(RunView const& run, float levelLength, std::vector<double> const* lstar, double lstarTotal,
    bool precise);

// Training.cpp
std::string startTraining(size_t index);
void stopTraining();
void trainingStep(PlayLayer* play);
void trainingRespawned();

// Ui.cpp
void resumeFromPause();
void openMenu();
void beginReplay(PlayLayer* play);
void askName(std::string const& title, std::string const& initial, std::function<void(std::string)> done);

// Library.cpp
void openLibrary();
void openSavedRuns(std::string const& key, std::string const& levelName);
void startPendingReplay(PlayLayer* play);

// LiveCounter.cpp
void captureLiveSnapshot(PlayLayer* play);
void finishLive();
void restoreLive(PlayLayer* play);
bool holdingJump(PlayerObject* player);
void liveStep(PlayLayer* play, int tick);
void dropLiveMeasures();
void tryStartLive();
void abortLive(std::string const& reason);

// Replay.cpp
void onBaselineDeath(Session& s, int deathTick, bool player1);
bool acceptBaseline(Session& s);
void finishAnalysis(PlayLayer* play);
void injectDueInputs(PlayLayer* play, GJBaseGameLayer* layer, Session& s);
void abortAnalysis(std::string const& reason);
void stopDriving(std::string const& reason);

// Display.cpp
void revealPassedInputs(PlayLayer* play);
void resetDisplay(PlayLayer* play);
std::vector<int> swiftLevelsOf(std::vector<InputEvent> const& events);
int swiftLevelAt(std::vector<InputEvent> const& events, size_t index);
bool showSymbols();
void refreshDisplay(PlayLayer* play);
std::vector<precision::TimedWindow> lstarInputs(RunView const& run, std::vector<size_t>* owners = nullptr);
void computeLStar();
bool hasEasierApproach(RunView const& run, size_t index);
std::string resultMarks(RunView const& run, size_t index);
double shownWindow(RunView const& run, size_t index);
std::vector<std::string> deepNotes(Session const& s, size_t index);

} // namespace tc
