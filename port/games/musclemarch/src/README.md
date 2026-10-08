# Hand-written game code

Readable C++ replacements for Muscle March's own functions, one folder per subsystem:
`input/` (controllers, poses, shaking), `race/` (the race), `scene/` (game flow, boot, menus),
and `debug/` (test tools such as the autoplay, not features). Each function is lifted with
`WP_LIFTED` from `wp/lift.h`, is listed in `../analysis/hle_functions.csv`, and is checked
against the translated original call by call (`WP_VERIFY=1 WP_VERIFY_FAST=1 WP_VERIFY_STACK=1`)
and against the golden replays (`make replay-test`).

Optional behavior changes do not go here: they are features (`../features/`, `docs/FEATURES.md`),
off unless the player opts in.
