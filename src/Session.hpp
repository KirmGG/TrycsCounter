#pragma once
// Runtime state shared by all hooks.

#include <Geode/Geode.hpp>

#include "ExactCheckpoints.hpp"
#include "ProbePlanner.hpp"
#include "WindowMath.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

namespace tc {

struct InputEvent {
    int tick = 0;
    bool down = false;
    int button = 0;
    bool player1 = true;
    float x = 0.f;
    float y = 0.f;
    // Registered between two physics steps (Click Between Steps). Such an input
    // cannot be reproduced by an on-step replay.
    bool betweenSteps = false;
    // Click Between Frames: the input was applied inside the player update of
    // physics step `tick`, after `frac` (0..1) of that step had been simulated.
    bool cbf = false;
    double frac = 0.0;
};

struct CheckpointInfo {
    int tick = 0;
    CheckpointObject* object = nullptr;
    // Size of the game's checkpoint list right after this checkpoint was added.
    // The game may add checkpoints of its own, so positions are tracked by index.
    size_t arrayCount = 0;
};

// A live measurement that waits for its verdict tick.
struct LiveJob {
    size_t index = 0;
    int verdictTick = 0;
};

// A saved game state taken shortly before a click (Live Counter keeps a short ring of them).
struct LiveSnapshot {
    int tick = 0;
    geode::Ref<CheckpointObject> object;
};

// Live Counter (trajectory mode): copies of the players taken during the attempt.
// A measurement simulates a copy forward with the recorded inputs, the way
// "show trajectory" tools do, instead of reloading the whole level.
struct TrajSnapshot {
    int tick = 0;
    geode::Ref<PlayerObject> p1;
    geode::Ref<PlayerObject> p2;
};

// Live Counter: state of a player at the start of a command tick.
struct LiveState {
    int tick = -1;
    float x = 0.f;
    float y = 0.f;
    double vy = 0.0;
    float size = 1.f;
    unsigned flags = 0;
};

// One simulated copy of a player inside a live measurement.
// Activation flags of an orb or pad (m_activated, by player 1, by player 2).
using ActivationFlags = std::array<bool, 3>;

struct SimRun {
    // Orbs and pads this copy used: each copy has its own, the level keeps its own.
    std::map<EnhancedGameObject*, ActivationFlags> used;
    geode::Ref<PlayerObject> sim;
    int tick = 0;
    InputEvent click;
    int shift = 0;
    size_t firstIndex = 0;      // first recorded input at or after the copy's start
    size_t applied = 0;         // planned inputs already applied
    int adaptMode = 0;          // correction attempt, see correctionShift
    int adaptAfterTick = 0;
    int deathTick = 0;
};

// Live Counter: the measurement of one click, run in step with the real attempt.
struct LiveMeasure {
    size_t index = 0;
    InputEvent event;
    bool player1 = true;
    int units = 1;
    size_t partner = std::string::npos;   // index of the release of a measured press, once known
    int startTick = 0;
    geode::Ref<PlayerObject> start;
    ProbePlanner planner;
    size_t plannerIndex = 0;
    SimRun control;
    SimRun trial;
    int trials = 0;
    bool done = false;
    std::string why;
};

enum class Stage {
    Idle,            // practice mode or nothing to record
    Recording,       // real attempt, inputs are recorded
    BaselineReplay,  // analysis: replay of the recorded run
    Probing,         // analysis: shifted-input trials
    Complete,        // analysis finished, waiting for the next reset
    Replay           // showing the counted run
};

enum class ProbeOutcome {
    None,
    Died,
    Survived
};

// State of one player at the start of a command tick.
struct PlayerTrace {
    float x = 0.f;
    float y = 0.f;
    double yVel = 0.0;
    float rotation = 0.f;
    float size = 1.f;
    float speed = 0.f;
    unsigned flags = 0;   // ground, gravity, game mode, held jump, ... (see playerTrace)
};

// Both players at the start of a command tick. Used to check that a replay
// follows the recorded attempt, that a trial started from a practice checkpoint
// follows the baseline replay exactly, and to see when a moved click has come
// back onto the baseline path.
struct TraceState {
    PlayerTrace p1;
    PlayerTrace p2;
    bool dual = false;
    bool valid = false;
    // Only the positions are known (a bot macro's Frame Fixes): no velocity,
    // flags, or player 2 unless `dual`.
    bool positionsOnly = false;
};

// Length of the player updates of one physics step, from this raw tick on until
// the next entry (only changes are stored). Normally constant, but Click Between
// Frames gives a step more than its length when an input carries a timestamp from
// before the frame (e.g. the release of the restart click at the level start):
// the player then moves 1.67 steps in one. Replays repeat the recorded length.
struct StepDt {
    int tick = 0;
    float p1 = 0.f;
    float p2 = 0.f;
};

// A correction of the player made by the replay (the mod's own "frame fix"):
// at raw tick `tick` the replay had the player at `from` while the recorded
// attempt had it at `to`. Small differences like this come from practice
// checkpoints (they restore the player a little differently than a continuous
// run) and from bots that set the player's position (xdBot Frame Fixes).
struct StateFix {
    int tick = 0;
    bool player2 = false;
    float fromX = 0.f;
    float fromY = 0.f;
    double fromVy = 0.0;
    float toX = 0.f;
    float toY = 0.f;
    double toVy = 0.0;
};

// A recorded attempt together with how it ended.
// A practice respawn during the recording: the attempt went on from the
// checkpoint at raw tick `tick`; the life before it had reached `reached`.
struct Respawn {
    int tick = 0;
    int reached = 0;
};

struct RunRecord {
    std::vector<InputEvent> events;
    int endTick = -1;
    RunEnd endKind = RunEnd::Death;
    // Raw ticks where the player touched a hazard but lived on (noclip).
    std::vector<int> hits;
    // State of the players per raw tick of this attempt: the path replay
    // corrections put the player back onto.
    std::vector<TraceState> trace;
    std::vector<StepDt> stepDts;
    // Where the attempt went on from a practice checkpoint.
    std::vector<Respawn> respawns;
    // Corrections the baseline replay needed to follow the attempt (sorted by tick).
    std::vector<StateFix> fixes;
    // A bot macro: no positions or trace were recorded; the baseline replay
    // fills in the positions and, while `openEnd`, finds where the run ends.
    bool fromMacro = false;
    bool openEnd = false;
    // X of the Start Pos the attempt began at; -1: the start of the level.
    float startX = -1.f;

    bool empty() const { return events.empty(); }
};

// One re-timed click of a correction chain: a moved click that dies only after
// the next click is fixed by re-timing that click, and if the run then dies
// after the click after it, that one is re-timed too (ship and wave players
// fix a mistake over several clicks).
struct AdaptLevel {
    size_t index = 0;     // the re-timed click
    int baseShift = 0;    // shift the candidates are built from (correctionShift)
    int attempt = 1;      // candidate being tried
    int deathTick = 0;    // the run died here before this click was re-timed
    int correction = 0;   // shift of the current candidate
};

// Deep Count result of one click (frames): the smallest and largest window
// with the previous click at other timings. `broken`: with the previous click
// moved, this one works neither at its time nor moved along with it.
struct DeepRange {
    double lo = 0.0;
    double hi = 0.0;
    bool broken = false;
    // The window the click usually has (typicalWindow), 0 if unknown.
    double typical = 0.0;
    // Disconnected window: its pieces (frames on the shift axis, earliest
    // first, more than one) and the step they were measured with.
    double step = 0.0;
    std::vector<WindowSpan> spans;

    // Easier approach: the widest window found with earlier inputs re-timed
    // (frames, 0: none better) and how ("the press before it 1.50 frames later").
    double easier = 0.0;
    std::string easierHow;

    bool ranged() const { return hi - lo > 1e-6; }
    bool disconnected() const { return spans.size() > 1; }
};

// One measurement of Deep Count: the window of `index` while the previous
// press `prev` is moved by `prevShift` measurement units (`weight`: how likely
// a player times the previous click there), or the holes of its window.
struct DeepJob {
    size_t index = 0;
    size_t prev = 0;
    int prevShift = 0;
    int variant = 0;    // 0: previous click moved, 2: holes, 3: edge check, 4: easier approach, 5: re-measure
    double weight = 1.0;
    // Variant 0: this click moves along with the previous one (same rhythm),
    // tried when it does not work at its recorded time any more.
    bool follow = false;
};

// Edge check: one trial of a measured window again, at its last passing
// (expect survive) or first failing (expect die) shift.
struct EdgeCheck {
    int shift = 0;
    bool expectSurvive = true;
    bool early = true;
};

// Easier approach search of one hard click (Deep Count): the earlier inputs are
// moved one after another (nearest first) to the edges of their windows; a move
// that widens the click is kept for the next ones.
struct ApproachMove {
    size_t input = 0;
    int shift = 0;
};
struct ApproachState {
    std::vector<size_t> inputs;
    size_t next = 0;
    std::vector<ApproachMove> keep;
    double base = 0.0;
    double best = 0.0;
    int pending = 0;
    double candidateBest = 0.0;
    int candidateShift = 0;
};

// What a Deep Count job with the previous click moved found.
struct DeepSample {
    double weight = 1.0;
    WindowResult result;
};

// Replay alignments tried when the baseline replay does not reproduce the
// recorded run. Values are raw ticks added to the configured offset.
inline constexpr std::array<int, 5> kAlignmentCandidates = {0, -1, 1, -2, 2};

// Score of an alignment after its baseline replay: died, or the raw tick where it
// left the recorded path (kAlignmentMatched when it never did).
inline constexpr int kAlignmentUntried = -2;
inline constexpr int kAlignmentDied = -1;
inline constexpr int kAlignmentMatched = std::numeric_limits<int>::max();

struct Session {
    Stage stage = Stage::Idle;
    int levelID = -1;
    std::string levelKey;   // see levelKeyOf: online ID, or a hash of a local level

    // GD does not restore its tick counter (m_tickIndex) when it loads a practice
    // checkpoint, it keeps counting. The mod remembers the tick of every
    // checkpoint and keeps its own clock: tick = m_tickIndex + tickOffset.
    std::unordered_map<CheckpointObject*, int> checkpointTicks;
    // What each checkpoint does not restore by itself (setting "Exact checkpoints"),
    // the checkpoint being loaded, and the orbs and pads of the level.
    std::unordered_map<CheckpointObject*, CheckpointExtra> checkpointExtras;
    // Macros counted one after another (Macros.cpp): the files still to count
    // and the name of the one being counted.
    std::deque<std::filesystem::path> macroQueue;
    std::string queuedMacro;
    // Training of one click (Training.cpp): the bot plays up to `stopTick`,
    // then the player tries the click from a practice checkpoint.
    struct Training {
        bool active = false;
        size_t index = 0;
        int stopTick = 0;
        int tries = 0;
        int inside = 0;
        bool judged = false;
        bool respawning = false;
    } training;
    CheckpointObject* loadingCheckpoint = nullptr;
    std::vector<EnhancedGameObject*> usableObjects;
    bool usableObjectsFound = false;
    int tickOffset = 0;
    bool resettingFromStart = false;

    // Recording is opt-in: the pause menu button "Start Recording" (or Live Counter) enables it.
    bool recordingEnabled = false;

    // Live Counter (practice mode): the window of every click is simulated shortly after it.
    std::deque<LiveJob> liveJobs;
    std::deque<LiveSnapshot> liveSnapshots;
    std::map<std::pair<int, bool>, bool> liveHeld;
    std::map<std::pair<int, bool>, bool> liveAnchorHeld;
    size_t liveBase = 0;        // index of the first recorded input inside the simulated slice
    size_t liveArrayBase = 0;   // size of the player's own checkpoint list
    CheckpointObject* liveAnchor = nullptr;
    geode::Ref<CheckpointObject> liveRestore;
    std::chrono::steady_clock::time_point liveStarted;
    size_t liveIndex = 0;
    int liveSavedAttempts = 0;
    bool liveSim = false;
    bool liveRestorePending = false;
    bool liveRestoring = false;
    bool liveDisabled = false;  // switched off for this level after a failed simulation
    bool liveHasResult = false;
    WindowResult liveResult;

    // Attempt in progress.
    std::vector<InputEvent> recording;
    std::map<std::pair<int, bool>, bool> recordHeld;
    int recordingDeathTick = -1;
    std::vector<int> recordingHits;
    std::vector<Respawn> recordingRespawns;
    // Noclip run: the current trial is only judged until the next recorded hit.
    int trialHitLimit = std::numeric_limits<int>::max();
    // Hits before this tick happen before the moved click and match the attempt.
    int trialHitFrom = 0;
    // Until this tick a trial must follow the recorded attempt exactly; if it
    // does not, its practice checkpoint did not restore the level faithfully.
    int trialVerifyUntil = std::numeric_limits<int>::min();
    // Practice checkpoints do not always restore the level exactly (moving
    // objects, long move triggers). Per checkpoint (raw tick): up to which raw
    // tick a run from it is known to follow the baseline, and the first raw tick
    // where a run from it left the baseline. A trial only starts from a
    // checkpoint that is proven up to the trial's end.
    std::map<int, int> checkpointGoodUntil;
    std::map<int, int> checkpointBadFrom;
    int badCheckpointRetries = 0;
    // The current run only checks a checkpoint: the recorded inputs unchanged,
    // compared with the baseline up to trialEndTick.
    bool verifyingCheckpoint = false;
    int checkpointControls = 0;
    // Correcting the next click after a moved click that died only after it:
    // 0 = no correction, k = attempt k of correctionShift.
    int adaptMode = 0;
    int adaptAfterTick = std::numeric_limits<int>::max();
    int lastProbeDeathTick = 0;
    int deathIgnoreUntilTick = 0;
    // After every moved input of a trial (raw tick), a trial whose players are
    // exactly in the baseline state replays the baseline from there on.
    int trialConvergeAfter = std::numeric_limits<int>::max();
    int trialConvergedSince = -1;
    int trialsConverged = 0;
    // The current trial survived only by coming back near the baseline path
    // ("Back on path"), an approximation.
    bool trialNear = false;
    // Outcomes (survived) of the normal count's trials that were decided
    // exactly, by input and shift: the edge check does not repeat them. Kept
    // with the results until the next count.
    std::map<std::pair<size_t, int>, bool> exactTrials;
    int checkpointRejections = 0;

    // Baseline replay: its own trace (the reference every trial is checked
    // against), the first raw tick where it left the recorded attempt, and the
    // score of every alignment tried.
    std::vector<TraceState> baselineTrace;
    int baselineDivergeTick = -1;
    std::array<int, kAlignmentCandidates.size()> alignmentScores{};
    bool baselineFinal = false;       // the best alignment was chosen, accept it
    int baselineMismatchTick = -1;    // accepted baseline left the recording here
    // The failed baseline replay that followed the attempt the longest: what
    // the error message describes.
    struct BaselineFailure {
        int reach = -1;       // raw tick where it left the attempt (or died)
        int deathTick = -1;
        int offset = 0;
        size_t cursor = 0;    // inputs replayed
    } bestFailure;

    // Finished attempts of the current level.
    RunRecord previousRun;
    RunRecord previousCompletion;

    // Analysis / replay of one run.
    RunRecord source;
    ProbePlanner planner;
    Trial activeTrial;
    std::vector<InputEvent> currentTrial;
    std::vector<CheckpointInfo> checkpoints;
    size_t replayCursor = 0;
    int lastInjectedTick = -1;
    int lastCheckpointTick = 0;
    int trialEndTick = 0;
    int trialsFinished = 0;
    int baseOffset = 0;
    int stepTicks = 2;       // shift granularity of the current measurement
    bool precise = false;    // windows are measured in fractions of a frame

    // Measurement units per raw tick (kCbfUnitsPerRawTick for Click Between Frames runs).
    int units = 1;

    // Click Between Frames recording: how much of the current physics step the
    // players have simulated, and the inputs of this step waiting for its total.
    int cbfAccTick = std::numeric_limits<int>::min();
    float cbfAcc1 = 0.f;
    float cbfAcc2 = 0.f;
    std::vector<std::pair<size_t, double>> cbfPending;

    // Live Counter, trajectory mode.
    std::deque<TrajSnapshot> trajSnapshots;
    std::vector<geode::Ref<PlayerObject>> trajPool;
    geode::Ref<PlayerObject> trajSim;
    std::vector<std::unique_ptr<LiveMeasure>> liveMeasures;
    // While a copy simulates one step: the real flags of the orbs and pads it touched.
    std::map<EnhancedGameObject*, ActivationFlags> simSavedFlags;
    std::map<EnhancedGameObject*, ActivationFlags>* simRunFlags = nullptr;
    std::vector<size_t> liveStartQueue;
    std::array<std::vector<LiveState>, 2> liveStates;
    bool trajSimulating = false;
    bool trajDead = false;
    float simStepDt = 0.25f;   // player update delta of one physics step

    // "Count Frame Windows" on the end screen waits for the level restart.
    std::optional<RunRecord> pendingAnalysis;
    // A reset the analysis asked for. It runs between two rendered frames, never
    // inside a physics step: resetting inside a step lets the game finish that
    // step on the reset state, which puts the replay one step ahead of its clock.
    enum class PendingReset { None, Level, FromStart };
    PendingReset pendingReset = PendingReset::None;

    // Click Between Frames replay: state of the step that was split by the mod.
    bool cbfMidStep = false;
    bool cbfSplitP1 = false;
    bool cbfSplitP2 = false;
    int cbfSplitTick = std::numeric_limits<int>::min();
    float cbfRotDelta = 0.f;
    cocos2d::CCPoint cbfP1Pos;
    cocos2d::CCPoint cbfP2Pos;

    // State of the players per raw tick of the attempt being recorded and of
    // the run being counted (the path replay corrections follow).
    std::vector<TraceState> trace;
    std::vector<StepDt> stepDts;     // recording: player step lengths (see StepDt)
    std::vector<TraceState> sourceTrace;

    size_t alignmentIndex = 0;
    bool injecting = false;
    bool startFromBeginning = false;
    bool replayDiedAtEnd = false;
    bool baselineRetry = false;
    // Layout mode (like Eclipse's): while counting, decoration is hidden (its
    // m_isHide before is kept to put it back) and visual-only triggers do not
    // fire. layoutOff: this count runs with the full level because the replay
    // did not match in layout mode.
    std::vector<std::pair<geode::Ref<GameObject>, bool>> layoutHidden;
    bool layoutActive = false;
    bool layoutOff = false;
    bool wasPracticeMode = false;

    // The attempt, jump and click counters (of the level and the player's
    // stats) from before a count or a replay: the bot's resets are not the
    // player's attempts. A replay also runs as a test run, so its completion
    // and percentage are not saved.
    struct PlayerStats {
        int attempts = 0;
        int jumps = 0;
        int clicks = 0;
        double attemptTime = 0.0;
        geode::SeedValueRSV levelAttempts;
        geode::SeedValueRSV levelJumps;
        geode::SeedValueRSV levelClicks;
        geode::SeedValueRSV levelAttemptTime;
        int totalJumps = 0;
        int totalAttempts = 0;
        bool testMode = false;
    };
    std::optional<PlayerStats> frozenStats;
    ProbeOutcome pendingOutcome = ProbeOutcome::None;

    // Results and what is currently shown.
    std::vector<WindowResult> windows;
    bool hasResults = false;
    int resultsLevelID = -1;
    std::string resultsKey;    // level the results belong to (levelKeyOf)
    bool resultsSaved = false; // the shown results are already in a saved run file
    size_t displayCursor = 0;
    std::array<int, kBucketCount> visibleCounts{};
    // Strong references: a label of a level that is gone stays safe to touch.
    std::array<geode::Ref<cocos2d::CCLabelBMFont>, kBucketCount> countLabels{};
    std::array<geode::Ref<cocos2d::CCLabelBMFont>, kBucketCount> bucketTextLabels{};
    geode::Ref<cocos2d::CCLabelBMFont> msLabel;
    geode::Ref<cocos2d::CCLabelBMFont> hzLabel;
    geode::Ref<cocos2d::CCLabelBMFont> progressLabel;
    geode::Ref<cocos2d::CCLabelBMFont> lstarLabel;
    bool hudPrecise = false;        // bucket scheme of the counter (ranges or whole frames)
    // Inputs at or before this raw tick were passed before the current respawn:
    // counted and marked without ding or animation.
    int displaySilentUntil = std::numeric_limits<int>::min();
    // Window of the last click shown (ms) and L* (sigma/s) up to it.
    double lastShownMs = 0.0;
    double lastShownLStar = 0.0;
    // Boxes (marker container space) of the rings and texts already placed:
    // a new marker puts its text where it covers none of them.
    std::vector<cocos2d::CCRect> markerBoxes;

    // Per input of the results: swift level (see swiftLevels) and L* after it.
    std::vector<int> sourceSwift;
    std::vector<double> lstar;      // NaN for inputs that are not counted
    double lstarTotal = 0.0;
    int lstarGeneration = 0;
    bool lstarReady = false;
    // The baseline replay of the results left the recorded attempt here (-1: never):
    // windows after it are marked approximate.
    int resultsMismatchTick = -1;
    std::string resultsName;        // name of the loaded saved run, if any
    // "~" per input: a shift of the previous click only survived when this one
    // was re-timed (found by the normal count, see adaptNext).
    std::vector<char> dependent;
    std::vector<char> edgeUncertain;   // per input: the edge check disagreed ("?")
    std::vector<DeepRange> deep;    // Deep Count per input

    // Deep Count in progress: windows of the normal count, the jobs (done from
    // the back, the checkpoints are trimmed from the end) and what they found.
    bool deepMode = false;
    std::vector<WindowResult> deepBase;
    std::vector<DeepJob> deepJobs;
    size_t deepJobsTotal = 0;
    bool deepJobActive = false;
    DeepJob deepCurrent;
    std::vector<std::vector<DeepSample>> deepSamples;      // per input
    std::vector<std::optional<WindowResult>> deepHoles;    // per input
    std::vector<double> deepBaseWeight;                    // weight of the recorded timing
    std::map<size_t, ApproachState> approach;              // easier approach search per hard click
    // Deep Count: the window of each click measured again with the deeper and
    // wider corrections (the base of its samples, holes and easier approach).
    std::vector<std::optional<WindowResult>> deepRebase;
    // Edge check, the first part of Deep Count (the windows wait in deepBase).
    bool verifyMode = false;
    bool edgeCheckRan = false;
    bool strictTrials = false;   // no "Back on path"
    std::vector<std::vector<EdgeCheck>> edgeChecks;
    std::vector<std::vector<int>> edgeOutcomes;
    // Inputs a trial starts from: the recorded ones, or with the previous click
    // moved for a Deep Count job.
    std::vector<InputEvent> trialBase;
    // Earliest raw tick any input of the current trial was moved from or to.
    int trialEarliestMove = 0;
    // Input re-timed by the correction attempts of the current trial.
    size_t adaptNext = std::string::npos;
    // Per recorded input: made in a free-flight mode (ship, wave, swing), where
    // a moved click only has to keep the player alive until the next click and
    // a moment to react (the player steers all the time).
    std::vector<char> freeFlight;
    // Correction chain of the current trial and the trials it has used.
    std::vector<AdaptLevel> adaptChain;
    int adaptTrials = 0;
    // Click after the last re-timed one: a death after it goes one level deeper.
    size_t adaptAfterIndex = std::string::npos;
    // Counting skips the drawing work of the level (updateVisibility) while
    // trials keep matching the baseline; switched off for the count otherwise.
    bool skipVisuals = false;
    int skipVisualsRejections = 0;

    // Replay corrections (StateFix): a run of consecutive real corrections
    // means the replay really went elsewhere; then it stops correcting.
    int fixRun = 0;
    int fixRunLastTick = -100;
    bool fixesGaveUp = false;
    float fixLargest = 0.f;
    // Practice recording: after a respawn the held buttons are compared with
    // the game once (the respawn may release them without an input).
    bool syncHeldAfterRespawn = false;

    // Live Counter (Exact) simulates with its own slice of the recording: the
    // counted results of the level are kept here meanwhile and put back after.
    struct LiveStash {
        RunRecord source;
        int units = 1;
        int stepTicks = 2;
        bool precise = false;
        bool hasResults = false;
        int baseOffset = 0;
        size_t alignmentIndex = 0;
    };
    std::optional<LiveStash> liveStash;

    // Audio muting while counting.
    bool audioMuted = false;
    float savedMusicVolume = 1.f;
    float savedSfxVolume = 1.f;

    static Session& get() {
        static Session value;
        return value;
    }

    bool isAnalyzing() const {
        return stage == Stage::BaselineReplay || stage == Stage::Probing;
    }

    // Real input is blocked and replaced by the recorded one.
    bool drivesInput() const {
        return isAnalyzing() || stage == Stage::Replay;
    }

    int activeOffset() const {
        return baseOffset + kAlignmentCandidates[std::min(alignmentIndex, kAlignmentCandidates.size() - 1)];
    }

    void resetProbeState() {
        currentTrial.clear();
        checkpoints.clear();
        replayCursor = 0;
        lastInjectedTick = -1;
        lastCheckpointTick = 0;
        trialEndTick = 0;
        trialHitLimit = std::numeric_limits<int>::max();
        trialHitFrom = 0;
        trialVerifyUntil = std::numeric_limits<int>::min();
        checkpointGoodUntil.clear();
        checkpointBadFrom.clear();
        badCheckpointRetries = 0;
        verifyingCheckpoint = false;
        checkpointControls = 0;
        adaptMode = 0;
        adaptAfterTick = std::numeric_limits<int>::max();
        adaptChain.clear();
        adaptTrials = 0;
        adaptAfterIndex = std::string::npos;
        skipVisuals = false;
        skipVisualsRejections = 0;
        lastProbeDeathTick = 0;
        trialConvergeAfter = std::numeric_limits<int>::max();
        trialConvergedSince = -1;
        trialsConverged = 0;
        checkpointRejections = 0;
        baselineTrace.clear();
        baselineDivergeTick = -1;
        alignmentScores.fill(kAlignmentUntried);
        baselineFinal = false;
        baselineMismatchTick = -1;
        bestFailure = {};
        trialsFinished = 0;
        alignmentIndex = 0;
        injecting = false;
        startFromBeginning = false;
        replayDiedAtEnd = false;
        baselineRetry = false;
        pendingReset = PendingReset::None;
        pendingOutcome = ProbeOutcome::None;
    }

    // Puts the player copies of the snapshots after `tick` back into the pool.
    void dropTrajSnapshotsAfter(int tick) {
        while (!trajSnapshots.empty() && trajSnapshots.back().tick > tick) {
            if (trajSnapshots.back().p1) trajPool.push_back(trajSnapshots.back().p1);
            if (trajSnapshots.back().p2) trajPool.push_back(trajSnapshots.back().p2);
            trajSnapshots.pop_back();
        }
    }

    void resetTrajectory() {
        liveMeasures.clear();
        liveStartQueue.clear();
        for (auto& ring : liveStates) ring.clear();
        trajSnapshots.clear();
        trajPool.clear();
        trajSim = nullptr;
        trajSimulating = false;
        trajDead = false;
    }

    void resetCbfStep() {
        cbfAccTick = std::numeric_limits<int>::min();
        cbfAcc1 = 0.f;
        cbfAcc2 = 0.f;
        cbfPending.clear();
        cbfMidStep = false;
        cbfSplitP1 = false;
        cbfSplitP2 = false;
        cbfSplitTick = std::numeric_limits<int>::min();
    }

    void resetLive() {
        liveJobs.clear();
        liveSnapshots.clear();
        liveHeld.clear();
        liveAnchorHeld.clear();
        liveAnchor = nullptr;
        liveRestore = nullptr;
        liveSim = false;
        liveRestorePending = false;
        liveRestoring = false;
        liveDisabled = false;
        liveHasResult = false;
    }

    void resetDisplay() {
        displayCursor = 0;
        visibleCounts.fill(0);
        lastShownMs = 0.0;
        lastShownLStar = 0.0;
        markerBoxes.clear();
    }

    void resetHudPointers() {
        for (auto& label : countLabels) label = nullptr;
        for (auto& label : bucketTextLabels) label = nullptr;
        msLabel = nullptr;
        hzLabel = nullptr;
        progressLabel = nullptr;
        lstarLabel = nullptr;
        resetDisplay();
    }

    size_t betweenStepInputs() const {
        size_t count = 0;
        for (auto const& event : source.events) count += event.betweenSteps ? 1 : 0;
        return count;
    }
};

} // namespace tc
