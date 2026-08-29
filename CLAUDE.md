# vibecut — handover notes

Read this first. `DEVLOG.md` in this same directory has the fuller
narrative/build-in-public version of the same history — this file is the
operational "pick up where we left off" doc.

## What this project is

A fork of [Kdenlive](https://kdenlive.org) that you can talk to — an AI
chat/terminal panel that can script and extend the editor live, not just
trigger pre-built features. This is the video-editing sibling to
[@10_X_eng](https://x.com/10_X_eng)'s [vibecad](https://github.com/10-X-eng/vibecad)
(a similarly AI-native fork of FreeCAD) — he challenged the user to do the
video equivalent, and the user wants to post build-in-public updates about
it in a similar style. When something notable lands, that's worth
drafting as a post, not just a commit.

## Architecture (modeled directly on vibecad)

Two authoring modes, not one big scripting blob:

1. **Native mode** — the LLM calls a curated set of existing
   Kdenlive/MLT operations (apply effect, insert clip, trim). Validated
   against the live project before executing. No code execution risk.
   This is where "remove the background noise from this clip" lives.
2. **VibeScript mode** — the LLM writes and runs actual code, in an
   isolated worker, and only validated output lands on the real project.
   This is the "recode it" part — genuine live extension. Planned as an
   embedded `QJSEngine` (Qt's own JS engine), not Python bindings — no
   binding-generation project needed since Kdenlive is already Qt/KDE.
   Saved scripts double as the addon/workflow system for free.

**Small context, deliberately.** vibecad gives its model the frozen
authoring surface, current selections, and stable IDs/revisions — not the
whole document dumped in every turn. Copy that constraint from day one.

Kdenlive has no real third-party plugin API today — `plugins/sampleplugin`
in the tree is a Qt Designer widget plugin, not a feature-extension point.
VibeScript is meant to *become* that mechanism, not sit next to a
separate one.

## Repo layout

- `upstream` remote = `https://github.com/kde/kdenlive` (for rebasing —
  they push `GIT_SILENT` sync commits daily, don't drift far)
- `origin` remote = `https://github.com/0xmeowmeow/vibecut` — a **real
  GitHub fork** (via `gh repo fork`, not a manually-created empty repo).
  Important: if origin ever needs recreating, use `gh repo fork
  kde/kdenlive --fork-name vibecut`, NOT `gh repo create --source=.` —
  the latter requires pushing the entire ~4.7GB upstream history yourself
  and GitHub will hang up the connection past ~2GB in one push. Forking
  server-side avoids transferring any of that.
- working branch: `vibecut`, off `master`

## Build

Debian trixie's own Qt (6.8.2) and MLT (7.30) are both too old for
Kdenlive (needs Qt 6.10+, MLT 7.38+). Building that stack by hand would be
a multi-day undertaking, so this uses Kdenlive's own Flatpak manifest
instead — same path their CI uses:

```
flatpak-builder --user --force-clean ~/data/programming/vibecut-buildir .flatpak-manifest.json
```

Needs `flatpak-builder` (apt) and `org.kde.Sdk//6.11` +
`org.kde.Platform//6.11` (flathub, `--user` install) — both already
installed on this machine.

**Two build gotchas already hit and fixed, don't rediscover them:**
1. flatpak-builder's `.flatpak-builder` state dir must be on the **same
   filesystem** as the build output dir — don't point the output dir at
   `/tmp` if the project lives elsewhere; it'll error immediately.
2. The manifest originally listed `org.freedesktop.Sdk.Extension.llvm21`
   under `sdk-extensions`. `org.kde.Sdk//6.11`'s generic extension point
   expects that at branch `6.11`, but flathub currently only publishes it
   at branch `25.08` (the underlying Freedesktop runtime version) — a
   live upstream SDK version-skew bug, not something we did wrong.
   `clang`/`llvm` aren't actually invoked anywhere in the build
   (confirmed by grep), so the fix was just to empty out
   `sdk-extensions: []` in `.flatpak-manifest.json` rather than fight
   Flatpak's branch resolution. If this breaks something later, that's
   where to look first.

**If you're starting a fresh session and the build already ran**: check
`~/data/programming/vibecut/build.log` and `ps -ef | grep flatpak-builder`
before starting a new one — it was last launched fully detached
(`setsid nohup ... &`, reparented to PID 1) specifically so it survives
a Claude Code session closing, so it may well have finished or still be
running from a previous session.

## Where things stood at last handover

Build was mid-compile (past all config issues, into the real dependency
chain — gavl, x264, MLT, Intel media SDK, glaxnimate, then Kdenlive
itself). Not yet confirmed the binary actually runs.

**Next steps, in order:**
1. Check `build.log` — if finished, verify the built Kdenlive actually
   launches (`flatpak-builder --run ~/data/programming/vibecut-buildir .flatpak-manifest.json kdenlive`
   or similar — check flatpak-builder docs for the exact run invocation).
2. First proof of concept: natural-language noise removal. Kdenlive
   already ships the effect (`data/effects/ladspa/ladspa_librnnoise.xml`)
   — this needs a chat dock panel + the smallest possible Native-mode
   command surface, not new video engineering. Proves the whole
   NL → agent → Kdenlive-action pipeline cheaply.
3. From there, build out the Native-mode command surface and the
   VibeScript sandbox incrementally.

## Feature wishlist (user's original list, bucketed by implementation strategy)

**Bucket A — already exists or near-free**, just needs a Native-mode
front end: background noise removal (RNNoise, already an effect).

**Bucket B — external tool/model orchestration**, lives behind
VibeScript-callable functions, not deep C++: auto color grading
(presets, not wheels), stock footage/image generation (Pexels + a gen
model), CLIP-based style matching against a reference video, Ollama/WebUI
interop, output/format optimization, YouTube upload.

**Bucket C — genuinely new subsystems**, each a project of its own, don't
bundle into the MVP: TUI mode (a whole second frontend), a Fusion-style
node compositor, CapCut-style meme templates. Third-party addons should
fall out of VibeScript's saved-script mechanism rather than needing a
separate system.

Also still open: the user's own question — "what takes the longest in a
real editing session?" — hasn't been answered yet. That should drive
which Bucket B/C item gets built first, rather than guessing.
