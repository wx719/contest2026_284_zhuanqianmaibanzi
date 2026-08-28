# VelaPoka Contest Project Guidance

## Session startup

Before changing contest code, read `DEVELOPMENT_PLAN.md`.  It is the source
of truth for the current hardware baseline, product architecture, milestone
status, and next concrete task.  Then inspect `git status` and preserve
unrelated user changes.

## Progress maintenance

After completing contest feature work, update these parts of
`DEVELOPMENT_PLAN.md`:

- last-updated date and current stage;
- hardware/status matrix;
- milestone checklist;
- current next step;
- change log.

Only mark a peripheral complete after its NuttX device node and a real-board
smoke test have both been verified.

## Source layout

- Maintained board source: `board/contest_board/`.
- Runtime mapping: `vendor/openvela/boards/contest2026_284_board/`.
- Planned product application source: `app/velapoka/`.
- Planned application mapping: `apps/examples/velapoka/`.
- Minimal recovery config: `board/contest_board/configs/nsh/`.
- Product config: `board/contest_board/configs/velapoka/`.

The maintained board source and runtime mapping can be hard linked in the
active workspace.  Check inode identity before editing both paths.
