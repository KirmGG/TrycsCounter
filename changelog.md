# Changelog

## v0.2.0-alpha

- Hitborders (experimental, after Deep Count): a replay in layout mode that shows where the players can be around every click, with a trail behind them and only the coloured counts
- The replay intro looks like NaN GD's, and shows "Raw Precision (24h) in σ/s" instead of the number
- Counter colours without CBF like NaN GD's: 1, 2, 3, 4, 5-6, 7-8 and the rest
- Marker colours blend between frames (0.5 is between pink and red)
- Training goes to the click sped up instead of playing the level up to it
- 2 player mode. In duals the marker is on the lower player, or on the other one when Deep Count finds its click more than 2 frames harder

## v0.1.2-alpha

- Fixed a crash when counting a long level (the fix in v0.1.1-alpha crashed the game once a count passed 50 checkpoints)

## v0.1.1-alpha

- Fixed counting on long levels: GD keeps only 50 practice checkpoints and drops the oldest ones, so after 50 the count loaded the wrong checkpoint and stopped with "After loading the checkpoint the level clock is at raw tick ..., expected ...". Counting now keeps all of them.

## v0.1.0-alpha

- First alpha
