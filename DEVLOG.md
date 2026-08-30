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

## 2026-08-30 — the panel could not have told you either way

Follow-up: still couldn't hear a difference from DeepFilterNet, even
with the "✓ Done." fix. Traced it to a real bug, not a listening
problem — `effect_apply` called `TimelineController::addEffectToClip()`,
which returns `void`, and then unconditionally returned `{"ok": true}`.
The tool had no way to know whether Kdenlive actually accepted the
effect; it just assumed. "✓ Done." was therefore possibly true, possibly
not — no way to tell from the code as it stood.

Fixed by going one layer lower: `TimelineModel::addClipEffect()` returns
the clip ids it actually touched, and `EffectStackModel::hasFilter()`
confirms the effect is really on the stack afterward. `effect_apply` now
only reports success once both check out, includes whether the effect
was already present (skips duplicate-adding), and a
`VibeCutAgent::toolFailed` signal surfaces a failure the instant it
happens rather than risking it get lost in an empty final reply.

Next: rerun against the cafe interview with this fix in and actually
confirm, via the Effect/Composition Stack panel, that the effect is
present — that's the ground truth no chat transcript claim can
substitute for.

## 2026-08-30 — found it: disabled thinking was dropping tool calls

The verification fix immediately paid off — instead of an unfalsifiable
"✓ Done.", the real run showed exactly what happened: `timeline_get_selection`
ran, then the turn ended with empty text and *no `effect_apply` call at
all*. Not called-and-failed — never called. Anthropic's own docs flag
this: with thinking disabled, Claude can end an agentic turn without
emitting a tool_use block it clearly intended to, no error raised. The
POC had shipped with thinking off (to keep the first version of the SSE
stream handler simpler) with a code comment to revisit it "once the loop
is proven" — it just proved itself unproven.

Flipped `thinking` from `{"type":"disabled"}` to `{"type":"adaptive"}`.
The stream handler already reconstructed and replayed thinking blocks
(with their signature) generically, so nothing else needed to change —
that generic handling had been written defensively for exactly this
flip. Rebuilt and installed; next real test is the cafe interview again.
It worked.

## 2026-08-30 — the Windsurf pivot, and subtitles end to end

Asked for subtitle generation next. Kdenlive already has real Whisper/Vosk
speech-to-text — this wasn't starting from nothing — but my first plan was
too timid: point the user at Kdenlive's own Settings dialog to install a
model first, then wire the chat trigger on top. The user pushed back hard,
and rightly: *"the chat panel tells the agent what to do, but the whole
program should be able to be operated through chat... go have a look at how
windsurf functioned."*

That's a real, standing shift in how the Native-mode tool surface should
grow — not a narrow allowlist requiring a GUI detour and a new PR for every
capability, but broad real control, the way Claude Code relates to a
repository rather than a fixed menu. Concretely: when Kdenlive's own code
already knows how to install its own dependencies, the agent should drive
that directly.

Traced the actual (non-dialog) entry points into Kdenlive's Python/pip
installer: `AbstractPythonInterface::installMissingDependencies()` for the
venv/pip bootstrap, `SpeechToTextWhisper::runConcurrentScript()` for the
model download — both genuinely async (`QFuture`-backed), neither pops a
window, unlike the dialog-wrapped `installNewModel()` the Settings UI uses.
One native KDE confirmation dialog is unavoidable the first time (Kdenlive's
own consent gate for an unattended multi-hundred-MB-to-GB download) — kept
that rather than routing around a deliberate upstream safety check, and
taught the model to warn the user about it.

Landed as three new tools:
- `speech_status` — installed/ready state, installed models, in-progress flag.
- `speech_setup` — drives the real installer + model download, returns
  immediately, progress arrives in the panel via a new
  `VibeCutTools::backgroundProgress` signal that isn't tied to any
  particular chat turn (setup can finish long after the turn that started
  it ended).
- `generate_subtitles` — exports the timeline's audio (mirroring
  `SpeechDialog`'s MLT multitrack render, kept the known "runs synchronously
  on the calling thread" limitation upstream already has, since it's
  audio-only and fast relative to the timeline), creates a subtitle track
  if none exists, runs Whisper as an async `QProcess` (this is the part
  that's actually slow, and it never blocks the GUI), and imports the
  resulting SRT via `SubtitleModel::importSubtitle()` on completion.

Next: run it for real on the cafe interview.

## 2026-08-30 — auto-target the clip, stop hiding what tools already know

First live compound request ("remove background noise/conversations and
add subtitles") surfaced two real gaps at once, both diagnosed straight
from the terminal logs the user pasted:

- Nothing was selected, and the timeline actually has an AV-split pair
  (Kdenlive's "split audio to a new track" feature: a video-only clip
  mirrored to an audio-only twin). The model applied the audio denoise
  effect to *both*. The audio one worked; the video-only one was always
  going to fail — `hasFilter()` correctly reported it, but nothing had
  ever taught the tool that a video-only clip can't host an audio
  effect in the first place. `resolveTargetClip()` now filters
  candidates by `PlaylistState` before resolving, and falls back to
  "the one clip on the timeline" instead of demanding a selection when
  there's no real ambiguity.
- The panel showed *that* `timeline_get_selection` and `speech_status`
  ran, never *what they answered*. If the model didn't narrate the
  result, the user had no way to know whether it hit a dead end. Fixed
  with a `toolCompleted` signal the dock renders unconditionally for
  read-only tools — ground truth the code already has, not something
  left to the model's discretion to mention.

Also traced a "why did it try to download again?" question to an actual
filesystem check (`~/.var/app/org.kde.kdenlive` has no venv, no whisper
cache) rather than guessing: nothing had ever actually installed before
— every earlier "checking setup" was a status check that correctly came
back negative, not a wasted repeat.
