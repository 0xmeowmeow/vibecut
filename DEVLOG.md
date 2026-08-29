# vibecut devlog

A fork of [Kdenlive](https://kdenlive.org) that you can talk to — an AI
chat/terminal panel that can script and extend the editor live, not just
call pre-built features. Taking on [@10_X_eng](https://x.com/10_X_eng)'s
challenge — the video-editing sibling to his
[vibecad](https://github.com/10-X-eng/vibecad) (FreeCAD).

Upstream: https://github.com/kde/kdenlive
Fork: https://github.com/0xmeowmeow/vibecut

## 2026-08-29 — kickoff

Cloned upstream `kde/kdenlive` (master), set up the fork with `upstream`/
`origin` remotes so we can rebase cleanly as Kdenlive moves (it's active —
daily `GIT_SILENT` sync commits from their translation/docs pipeline).

**The core idea**, modeled directly on vibecad's two-mode split:

- **Native mode** — the LLM calls a curated set of existing Kdenlive/MLT
  operations (apply an effect, insert a clip, trim). Validated against the
  live project before it runs, no code execution. This is where "remove
  the background noise from this clip" lives — and it turns out Kdenlive
  already ships an RNNoise effect (`data/effects/ladspa/ladspa_librnnoise.xml`),
  so the first proof of concept barely needs new video engineering at all,
  just a way to route natural language into it.
- **VibeScript mode** — the LLM actually writes and runs code, in an
  isolated worker, and only validated output lands on the real project.
  This is the "recode it" part — genuine live extension of the editor,
  not just calling fixed functions. Planned as an embedded `QJSEngine`
  (Qt's own JS engine) rather than Python bindings, since Kdenlive is
  already Qt/KDE and this avoids a whole binding-generation project.
  Saved scripts double as the addon/workflow system — a "workflow" is
  just a named, shareable script.

Kdenlive doesn't have a real third-party plugin API today (the
`plugins/sampleplugin` in the tree is a Qt Designer widget plugin, not a
feature-extension point) — so the addon story has to be built, and
VibeScript is designed to be that mechanism rather than a separate system.

**Build**: needs Qt 6.10+, KF6 6.21+, MLT 7.38+ — all newer than what
Debian trixie ships (Qt 6.8.2, MLT 7.30). Rather than hand-building that
whole stack from source, using Kdenlive's own Flatpak manifest
(`.flatpak-manifest.json`, `org.kde.Sdk//6.11`) — this is the same path
their own CI uses, and sidesteps days of bootstrapping Qt/KF6 by hand.

**Not yet decided**: what the chat panel's actual API surface looks like
beyond the noise-removal proof of concept, and how much of the wishlist
(auto color grading, stock footage/generation, CLIP style-matching,
YouTube upload, Fusion-style node compositor, TUI mode) becomes
VibeScript-callable functions vs. dedicated Native-mode commands.

## 2026-08-29 — fork fixed, build in progress

First fork attempt was wrong: created an empty GitHub repo and tried to
push the full ~4.7GB local history into it directly — GitHub hangs up
past ~2GB in a single push. Fixed by using `gh repo fork kde/kdenlive
--fork-name vibecut` instead, which forks server-side with zero data
transfer, then pushed just the new `vibecut` branch (1.9KB) on top.

Build via `flatpak-builder` against `org.kde.Sdk//6.11` hit two real
snags: the state dir needs to be on the same filesystem as the build
output dir, and the manifest's `org.freedesktop.Sdk.Extension.llvm21`
requirement doesn't currently resolve (KDE's SDK 6.11 wants it at branch
`6.11`, flathub only publishes `25.08`) — a live upstream version-skew
bug, not us. Since `clang`/`llvm` aren't actually invoked by the build,
dropped that extension requirement entirely and the build proceeded past
config into the real dependency compile (gavl, x264, MLT, Intel media
SDK, glaxnimate, ...).

Handed the build off to a fully-detached process (`setsid nohup`,
reparented to PID 1) before closing out this session, specifically so a
multi-hour compile survives a Claude Code restart. See `CLAUDE.md` for
the full operational handover notes.
