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
    // Registered between two physics steps (Click Between Steps).
    bool betweenSteps = false;
    // Click Between Frames: the input was applied inside the player update of physics step `tick`.
    bool cbf = false;
    double frac = 0.0;
    // A dual played by one player: x, y is the lower player, this the other one.
    bool dual = false;
    bool otherP2 = false;
    float ox = 0.f;
    float oy = 0.f;
};

struct CheckpointInfo {
    int tick = 0;
    CheckpointObject* object = nullptr;
    // Size of the game's checkpoint list right after this checkpoint was added.
    size_t arrayCount = 0;
};

// A live measurement that waits for its verdict tick.
struct LiveJob {
    size_t index = 0;
    int verdictTick = 0;
};

// A saved game state taken shortly before a click.
struct LiveSnapshot {
    int tick = 0;
    geode::Ref<CheckpointObject> object;
};

// Live Counter (trajectory mode): copies of the players taken during the attempt.
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

// Both players at the start of a command tick.
struct TraceState {
    PlayerTrace p1;
    PlayerTrace p2;
    bool dual = false;
    bool valid = false;
    // Only the positions are known: no velocity, flags, or player 2 unless `dual`.
    bool positionsOnly = false;
};

// Length of the player updates of one physics step, from this raw tick on until the next entry.
struct StepDt {
    int tick = 0;
    float p1 = 0.f;
    float p2 = 0.f;
};

// A correction of the player made by the replay.
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
    // State of the players per raw tick of this attempt.
    std::vector<TraceState> trace;
    std::vector<StepDt> stepDts;
    // Where the attempt went on from a practice checkpoint.
    std::vector<Respawn> respawns;
    // Corrections the baseline replay needed to follow the attempt (sorted by tick).
    std::vector<StateFix> fixes;
    // A bot macro: no positions or trace were recorded.
    bool fromMacro = false;
    bool openEnd = false;
    // X of the Start Pos the attempt began at; -1: the start of the level.
    float startX = -1.f;

    bool empty() const { return events.empty(); }
};

// One re-timed click of a correction chain.
struct AdaptLevel {
    size_t index = 0;     // the re-timed click
    int baseShift = 0;    // shift the candidates are built from (correctionShift)
    int attempt = 1;      // candidate being tried
    int deathTick = 0;    // the run died here before this click was re-timed
    int correction = 0;   // shift of the current candidate
};

// Deep Count result of one click.
struct DeepRange {
    double lo = 0.0;
    double hi = 0.0;
    bool broken = false;
    // The window the click usually has (typicalWindow), 0 if unknown.
    double typical = 0.0;
    // Disconnected window: its pieces and the step they were measured with.
    double step = 0.0;
    std::vector<WindowSpan> spans;

    // Easier approach: the widest window found with earlier inputs re-timed and how.
    double easier = 0.0;
    std::string easierHow;

    bool ranged() const { return hi - lo > 1e-6; }
    bool disconnected() const { return spans.size() > 1; }
};

// One measurement of Deep Count.
struct DeepJob {
    size_t index = 0;
    size_t prev = 0;
    int prevShift = 0;
    int variant = 0;    // 0: previous click moved, 2: holes, 3: edge check, 4: easier approach, 5: re-measure,
                        // 6: hitborders, 7: a dual's lower player alone
    double weight = 1.0;
    // Variant 0: this click moves along with the previous one.
    bool follow = false;
};

// Edge check: one trial of a measured window again, at its last passing or first failing shift.
struct EdgeCheck {
    int shift = 0;
    bool expectSurvive = true;
    bool early = true;
};

// Easier approach search of one hard click.
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

// Where a player was at a raw tick, in object layer space.
struct PathPoint {
    int tick = 0;
    float x = 0.f;
    float y = 0.f;
};

// Make Hitborders: the run's own path and, for every counted click.
struct Hitborders {
    std::vector<PathPoint> path1;
    std::vector<PathPoint> path2;   // the second player of a dual
    // input: early and late path of player 1, then of player 2 (in a dual)
    std::map<size_t, std::array<std::vector<PathPoint>, 4>> clicks;

    bool empty() const { return clicks.empty(); }
};

// Replay alignments tried when the baseline replay does not reproduce the recorded run.
inline constexpr std::array<int, 5> kAlignmentCandidates = {0, -1, 1, -2, 2};

// Score of an alignment after its baseline replay.
inline constexpr int kAlignmentUntried = -2;
inline constexpr int kAlignmentDied = -1;
inline constexpr int kAlignmentMatched = std::numeric_limits<int>::max();

struct Session {
    Stage stage = Stage::Idle;
    int levelID = -1;
    std::string levelKey;   // see levelKeyOf: online ID, or a hash of a local level

    // GD does not restore its tick counter when it loads a practice checkpoint, it keeps counting.
    std::unordered_map<CheckpointObject*, int> checkpointTicks;
    // What each checkpoint does not restore by itself, the checkpoint being loaded.
    std::unordered_map<CheckpointObject*, CheckpointExtra> checkpointExtras;
    // Macros counted one after another: the files still to count and the name of the one being counted.
    std::deque<std::filesystem::path> macroQueue;
    std::string queuedMacro;
    // Training of one click.
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

    // Recording is opt-in: the pause menu button "Start Recording" enables it.
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
    // Until this tick a trial must follow the recorded attempt exactly.
    int trialVerifyUntil = std::numeric_limits<int>::min();
    // Practice checkpoints do not always restore the level exactly.
    std::map<int, int> checkpointGoodUntil;
    std::map<int, int> checkpointBadFrom;
    int badCheckpointRetries = 0;
    // The current run only checks a checkpoint.
    bool verifyingCheckpoint = false;
    int checkpointControls = 0;
    // Correcting the next click after a moved click that died only after it.
    int adaptMode = 0;
    int adaptAfterTick = std::numeric_limits<int>::max();
    int lastProbeDeathTick = 0;
    int deathIgnoreUntilTick = 0;
    // After every moved input of a trial.
    int trialConvergeAfter = std::numeric_limits<int>::max();
    int trialConvergedSince = -1;
    int trialsConverged = 0;
    // The current trial survived only by coming back near the baseline path, an approximation.
    bool trialNear = false;
    // Outcomes of the normal count's trials that were decided exactly, by input and shift.
    std::map<std::pair<size_t, int>, bool> exactTrials;
    // Which player died in a failed trial of (input, shift): true for player 2.
    std::map<std::pair<size_t, int>, bool> trialDeathP2;
    bool lastDeathP2 = false;
    // Deep Count, duals: the player that cannot die while the other one is measured alone.
    int immunePlayer = 0;
    std::vector<char> otherMarker;
    // Deep Count ran on these results (Make Hitborders needs it).
    bool deepCounted = false;
    // Make Hitborders: the pass is running, the shifts of every click, the path of the current trial.
    bool borderMode = false;
    std::map<size_t, std::vector<int>> borderShifts;
    std::array<std::vector<PathPoint>, 2> borderTrial;
    // The farthest shift kept so far per.
    std::map<std::pair<size_t, int>, int> borderBest;
    Hitborders borders;
    // The replay of the hitborders.
    bool pendingBorderReplay = false;
    bool borderReplay = false;
    bool borderReplayRan = false;
    bool layoutLoad = false;
    // The trail behind the players while the hitborders are shown.
    int trailTick = -1;
    std::array<std::optional<cocos2d::CCPoint>, 2> trailLast;
    int checkpointRejections = 0;

    // Baseline replay: its own trace, the first raw tick where it left the recorded attempt.
    std::vector<TraceState> baselineTrace;
    int baselineDivergeTick = -1;
    std::array<int, kAlignmentCandidates.size()> alignmentScores{};
    bool baselineFinal = false;       // the best alignment was chosen, accept it
    int baselineMismatchTick = -1;    // accepted baseline left the recording here
    // The failed baseline replay that followed the attempt the longest: what the error message describes.
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

    // Click Between Frames recording.
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
    // A reset the analysis asked for.
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

    // State of the players per raw tick of the attempt being recorded and of the run being counted.
    std::vector<TraceState> trace;
    std::vector<StepDt> stepDts;     // recording: player step lengths (see StepDt)
    std::vector<TraceState> sourceTrace;

    size_t alignmentIndex = 0;
    bool injecting = false;
    bool startFromBeginning = false;
    bool replayDiedAtEnd = false;
    bool baselineRetry = false;
    // Layout mode: while counting, decoration is hidden and visual-only triggers do not fire.
    std::vector<std::pair<geode::Ref<GameObject>, bool>> layoutHidden;
    bool layoutActive = false;
    bool layoutOff = false;
    bool wasPracticeMode = false;

    // The attempt, jump and click counters from before a count or a replay.
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
    // Inputs at or before this raw tick were passed before the current respawn.
    int displaySilentUntil = std::numeric_limits<int>::min();
    // Window of the last click shown (ms) and L* (sigma/s) up to it.
    double lastShownMs = 0.0;
    double lastShownLStar = 0.0;
    // Boxes of the rings and texts already placed.
    std::vector<cocos2d::CCRect> markerBoxes;

    // Per input of the results: swift level (see swiftLevels) and L* after it.
    std::vector<int> sourceSwift;
    std::vector<double> lstar;      // NaN for inputs that are not counted
    double lstarTotal = 0.0;
    int lstarGeneration = 0;
    bool lstarReady = false;
    // The baseline replay of the results left the recorded attempt here.
    int resultsMismatchTick = -1;
    std::string resultsName;        // name of the loaded saved run, if any
    // "~" per input: a shift of the previous click only survived when this one was re-timed.
    std::vector<char> dependent;
    std::vector<char> edgeUncertain;   // per input: the edge check disagreed ("?")
    std::vector<DeepRange> deep;    // Deep Count per input

    // Deep Count in progress: windows of the normal count, the jobs and what they found.
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
    // Deep Count: the window of each click measured again with the deeper and wider corrections.
    std::vector<std::optional<WindowResult>> deepRebase;
    // Edge check, the first part of Deep Count (the windows wait in deepBase).
    bool verifyMode = false;
    bool edgeCheckRan = false;
    bool strictTrials = false;   // no "Back on path"
    std::vector<std::vector<EdgeCheck>> edgeChecks;
    std::vector<std::vector<int>> edgeOutcomes;
    // Inputs a trial starts from.
    std::vector<InputEvent> trialBase;
    // Earliest raw tick any input of the current trial was moved from or to.
    int trialEarliestMove = 0;
    // Input re-timed by the correction attempts of the current trial.
    size_t adaptNext = std::string::npos;
    // Per recorded input: made in a free-flight mode.
    std::vector<char> freeFlight;
    // Correction chain of the current trial and the trials it has used.
    std::vector<AdaptLevel> adaptChain;
    int adaptTrials = 0;
    // Click after the last re-timed one: a death after it goes one level deeper.
    size_t adaptAfterIndex = std::string::npos;
    // Counting skips the drawing work of the level while trials keep matching the baseline.
    bool skipVisuals = false;
    int skipVisualsRejections = 0;

    // Replay corrections: a run of consecutive real corrections means the replay really went elsewhere.
    int fixRun = 0;
    int fixRunLastTick = -100;
    bool fixesGaveUp = false;
    float fixLargest = 0.f;
    // Practice recording: after a respawn the held buttons are compared with the game once.
    bool syncHeldAfterRespawn = false;

    // Live Counter simulates with its own slice of the recording.
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

    // Training: the bot runs up to the click sped up, behind the counting screen.
    bool trainingFastForward() const {
        return training.active && stage == Stage::Replay;
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
