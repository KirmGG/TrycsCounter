# TrycsCounter

<co>This mod is in Alpha and is being actively developed.</co> Found a bug or have an idea how to make it better? Message me on Discord: <cy>kirmgg</cy>

## What is it

The Frame Window Counter a lot of people know, made as close to NaN GD's as possible. It shows how many frames you had for every click in your run: rings with the window at each click, the counter, the ding and L*.

For even more accurate results, run <cg>Deep Count</cg> after the normal count. It takes longer though.

## How to use

1. Pause > <cy>TC</cy> > **Start Recording** (it turns on practice mode), then play an attempt
2. Pause > <cy>TC</cy> > **Count Windows** and wait (don't click while it counts)
3. Press **Replay** and watch your run with all the windows
4. Optional: <cy>TC</cy> > <cg>Deep Count</cg>

## Features

- Counts your own attempts (normal mode, practice, noclip) or bot macros (`.gdr`, `.gdr.json`, `.gdr2`)
- Click Between Frames and Click Between Steps support
- <cg>Deep Count</cg>: clicks that depend on the previous one, split windows, easier approaches
- Replay with an intro card, like in NaN GD videos
- The counter shows up in your normal attempts too
- Graph of the run and click training: practise any hard click from a checkpoint
- Saved runs, and a library in the main menu with every level you counted
- Show frame windows right in the editor
- Hitborders after Deep Count: where you can be around every click <cr>(experimental)</c>
- 2 player mode and duals
- Live Counter <cr>(experimental)</cr>

## Symbols after a window

- `~` depends on the previous click
- `-` the window is split in parts, the click works again after a gap
- `!` `!!` `!!!` swift, double swift, triple swift click
- `^` there is an easier way to time the clicks before it
- `+` the window hit a limit and can be wider
- `?` <cr>might be inaccurate</cr>

`-` and `^` come from <cg>Deep Count</cg>. During the replay, clicks like these get a short note at the top right.

## How it works

**Recording.** The mod saves every press and release exactly as the game got it, including where between physics steps it landed with CBF.

**Counting.** First the mod replays your whole run and checks that it matches your attempt, placing checkpoints along the way. Then it goes from the last click to the first: loads a checkpoint, moves one click earlier or later frame by frame, and checks if you would still survive. The window is how many frames in a row pass around your click.

**Survival.** A moved click is fine if you survive until the next click and a bit more. If you die only after the next click, the mod tries to fix it with the next clicks, like a real player would. In ship, wave and swing you only need to survive until the next click, because you steer all the time there.

**<cg>Deep Count</cg>.** Checks every window edge again strictly, measures each click with the previous click timed differently, looks a few frames past the edges for a second window, and searches for an easier approach on hard clicks.

**Hitborders** <cr>(experimental)</c>. Every click is replayed at the early and the late edge of its window. Then the run is replayed in layout mode: the orange band between those two paths is where you can be, a white trail follows you, and the counter shows only its coloured numbers.

**Speed.** While counting, the game runs much faster than normal (how fast depends on your CPU), the level is hidden and decoration is turned off. Everything is back to normal after.

## Limits

- GD 2.2081 on Windows, Android and iOS, no platformer
- <cr>Random triggers can break the count</cr>: the mod warns you about them
- Turn off Physics Bypass in Click Between Frames

## Credits

The checkpoint tick restore follows the approach of [Frame Window Counter](https://github.com/hyper-5/frame-window-counter) by hyper-5. The counter look is based on NaN GD's videos.
