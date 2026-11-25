# Changelog

## v0.1.2-alpha

- Fixed a crash when counting a long level (the fix in v0.1.1-alpha crashed the game once a count passed 50 checkpoints)

## v0.1.1-alpha

- Fixed counting on long levels: GD keeps only 50 practice checkpoints and drops the oldest ones, so after 50 the count loaded the wrong checkpoint and stopped with "After loading the checkpoint the level clock is at raw tick ..., expected ...". Counting now keeps all of them.

## v0.1.0-alpha

- First alpha
