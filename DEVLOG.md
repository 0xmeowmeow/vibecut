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

**Build finished clean.** Installed locally (`flatpak-builder --install`)
and confirmed it actually runs: `flatpak run org.kde.kdenlive//master`
reports `kdenlive 26.11.70`. Shows up in app launchers (wofi etc.) as
"Kdenlive (Nightly)" — the manifest's `desktop-file-name-suffix` keeps it
distinct from any stock Kdenlive install. First milestone done: we have
a working build of our own fork to actually develop against.

Next: the noise-removal proof of concept (chat dock panel + smallest
possible Native-mode command surface calling the existing RNNoise
effect).

## 2026-08-30 — the assistant panel, wired end to end

Read through vibecad's actual AI subsystem (`src/Mod/VibeCAD/`, ~776k
lines) to copy its decisions rather than reinvent them. What carried
over directly: the tiny byte-capped system prompt sent as a cached
system block; the per-tool JSON-Schema spec + `{"ok": bool, ...}` result
contract (their `tool_impl/service/*` modules); an **allowlist** as the
guard rail for what the model can actually touch (their
`NativeActionManifest`); provider work kept off the UI thread with tool
execution marshalled back to it; and `claude-sonnet-5` as the default
model.

The one decision that didn't port: vibecad runs the `anthropic` **Python**
SDK in a child process because FreeCAD is already Python. Kdenlive is
C++/Qt with no Python layer, so the equivalent here is a pure-Qt client —
`QNetworkAccessManager` streams `POST /v1/messages` (SSE), a small
incremental parser rebuilds the assistant message from the event stream,
and on `stop_reason == "tool_use"` the loop runs the requested tools and
feeds results back. It's event-driven on the GUI thread, so no worker
thread is needed. Chose this over a bundled Python sidecar to keep
vibecut single-language and add nothing to the (rebased-often, fragile)
Flatpak manifest.

Landed in `src/vibecut/`:

- `sseparser.h` — header-only, dependency-free SSE stream parser
  (buffers partial records across arbitrary network chunk boundaries).
- `vibecutagent.{h,cpp}` — the Anthropic client + tool-use loop.
- `vibecuttools.{h,cpp}` — the Native-mode tool surface: `timeline_list_clips`,
  `timeline_get_selection`, `effect_apply` (allowlisted; `denoise` →
  `ladspa.9354877`, the RNNoise effect), `ask_user`.
- `vibecutdock.{h,cpp}` — the panel: transcript + prompt line, registered
  on the main window as a KDDockWidgets panel next to Library / Markers /
  Speech Editor.
- `tests/vibecuttest.cpp` — Catch2 tests for the SSE parser, the effect
  allowlist, and the tool-schema shape.

API key for this first cut comes from `ANTHROPIC_API_KEY` in the
environment; KWallet + a settings page come later.

Build snags: one real bug (`qAsConst` needs `<QtGlobal>`; dropped it and
iterated the array directly) and one self-inflicted — adding `--ccache`
to the flatpak-builder invocation changed the module cache key and forced
MLT / OTIO / kddockwidgets to rebuild once. Both fixed; the fork now
builds, installs, and runs (`kdenlive 26.11.70`), and the SSE parser's
tricky cases (split mid-token, multiple events per chunk, CRLF,
multi-line `data:`) are verified.

Not yet done: driving the whole loop against the live API with a real
key and a real project — "remove background noise from the selected
clip" landing the RNNoise effect on the clip's stack.

## 2026-08-30 — DeepFilterNet, because RNNoise wasn't enough

First live run worked — the tool loop fired and dropped the effect on the
clip — but on a real interview recorded in a busy cafe you couldn't hear a
difference. That's RNNoise doing what RNNoise does: it's trained on
*stationary* noise (hiss, hum, HVAC), and cafe babble — other people
talking, cutlery, music — is the exact case it can't touch, because it
can't tell the target voice from background voices.

So bundled **DeepFilterNet** (DFN3, `Rikorose/DeepFilterNet` v0.5.6) — a
deep-learning speech denoiser that handles non-stationary noise. It ships
a **LADSPA plugin**, which means it drops straight into the same slot
Kdenlive already uses for the RNNoise effect — no subprocess, no file
juggling, non-destructive, real-time preview:

- `packaging/flatpak/org.kde.kdenlive-dependencies.json` — new
  `deepfilternet-ladspa` module fetches the prebuilt
  `libdeep_filter_ladspa.so` (glibc build, standard deps only) into
  `/app/lib/ladspa`. Placed last in the dep list so it doesn't invalidate
  the cache of anything else.
- `data/effects/ladspa/ladspa_deepfilternet.xml` — Kdenlive effect
  wrapper, "AI Noise Removal (DeepFilterNet)", LADSPA id `ladspa.7843795`
  (mono), exposing Attenuation Limit + the processing thresholds.
- `vibecuttools.cpp` — `denoise` in the allowlist now maps to DFN;
  the old RNNoise filter stays as `denoise_light`. The `effect_apply`
  tool description tells the model to reach for `denoise` on real-world
  location noise.

Verified end to end with `melt` inside the Flatpak: `analyseplugin` sees
the plugin (id, ports, defaults all as encoded in the XML), and running
`-filter ladspa.7843795` over a test signal processes the audio — on a
non-speech tone+noise mix at max attenuation it drops the output ~70 dB,
i.e. it's aggressively removing everything it doesn't classify as voice.
On a real voice recording it keeps the voice and takes out the room.

Same next step as before, now with a denoiser that will actually show a
difference: run the whole NL loop live against the cafe interview.

## 2026-08-30 — the panel becomes an actual chat surface

First live-use feedback, two things: a bolted-on "Remove background
noise" button wasn't the right shape — the expected pattern for this
kind of tool (Claude Code, Windsurf, "vibe coding" chat panels
generally) is a welcome message with clickable suggestions *inside* the
transcript, not separate buttons. And it wasn't clear a request had even
run — the only feedback for a tool-only turn was a raw
`→ effect_apply {...}` line, and the system prompt's "keep replies
short" instruction meant the model could end a turn with no text at all.

Fixed both. `QTextEdit` → `QTextBrowser` for the transcript so
suggestions render as real clickable links (`anchorClicked`); on load it
shows a greeting plus three suggestions — remove background noise
(still selection-gated, same auto-apply-when-you-click-a-clip flow),
list the timeline's clips, and a generic "what can you help me with".
Clicking one sends that exact prompt. Tool calls now narrate in plain
language ("Adding \"AI Noise Removal (DeepFilterNet)\"…") instead of raw
JSON. An indeterminate progress bar shows next to the status label
while a request is in flight. And critically: a turn that ends with no
assistant text now always leaves a visible "✓ Done." line, so "did it
run?" always has an answer on screen.
