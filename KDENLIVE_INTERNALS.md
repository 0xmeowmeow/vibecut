# Kdenlive internals — reference notes

Not a full architecture graph — targeted notes on the specific subsystems
vibecut has had to reverse-engineer to drive Kdenlive from code, grown as
we go rather than built upfront. Each section: what it is, the real
working call path (not the plausibly-named method), and the traps
already hit. Add a section here whenever a new subsystem gets touched —
that's the payoff, not a one-time exhaustive pass.

## Timeline clips and effects

- A clip on the timeline is an integer id. `TimelineModel::isClip(id)`
  validates it; `getClipName`, `getClipTrackId`, `getClipPosition`,
  `getClipPlaytime`, `getClipBinId` read it.
- **Adding an effect**: `TimelineController::addEffectToClip()` returns
  `void` — it cannot tell you whether Kdenlive actually accepted the
  effect. Call `TimelineModel::addClipEffect(clipId, effectId)` directly
  instead (it returns the clip ids it actually touched), then confirm
  via `TimelineModel::getClipEffectStack(clipId)->hasFilter(effectId)`.
  Never trust "the call didn't throw" as success.
- **AV-split pairs**: Kdenlive's "split audio to a new track" feature
  produces two linked clips from one source — a video-only clip and an
  audio-only mirror (`mirrorTrack` attribute in the project XML), joined
  by an `AVSplit` group. Their `state` is `PlaylistState::ClipState`:
  `VideoOnly = 1`, `AudioOnly = 2`, `Disabled = 3`, `Unknown = 4`
  (`src/definitions.h`). **An audio effect can never land on the
  video-only half** — `TimelineModel::getClipState(clipId).first` tells
  you which is which. Filter candidates by this *before* attempting to
  apply, not after failing.
- When resolving "which clip" with no explicit id and nothing selected:
  if there's exactly one *eligible* clip on the timeline, use it — don't
  force a selection for the common single-clip case. Multiple eligible
  candidates with nothing selected is real ambiguity; list them so the
  question asked back is specific.

## LADSPA audio effects

- Kdenlive's XML effect id convention is `ladspa.<uniqueid>` — the
  LADSPA plugin's unique ID from its `ladspa_descriptor()`. MLT finds the
  plugin by scanning `LADSPA_PATH` (set in `.flatpak-manifest.json`'s
  `finish-args`) for any `.so` whose unique id matches; filename doesn't
  matter.
- Effect XML `<parameter name="N">` uses the **absolute LADSPA port
  index** (0 = first audio in, 1 = first audio out for a mono plugin,
  controls start after the audio ports) — not a control-port-relative
  index. Confirmed against both the shipped RNNoise effect and a custom
  one (DeepFilterNet, `ladspa.7843795`).
- `analyseplugin` (from the `ladspa-sdk` package, present in the Flatpak
  runtime) dumps a `.so`'s exact port layout, ids, and defaults — use it
  to write the effect XML instead of guessing from the plugin's source.
- `melt -filter ladspa.<id> <params> -consumer ...` runs a LADSPA effect
  standalone, headless, outside Kdenlive entirely — the fastest way to
  confirm a new effect actually processes audio before wiring any C++.

## Subtitle track lifecycle

- A project has **no subtitle track by default**;
  `TimelineModel::getSubtitleModel()` returns `nullptr` until one exists.
  `TimelineModel::hasSubtitleModel()` checks; `TimelineItemModel::
  createSubtitleModel()` creates one (mirrors
  `MainWindow::slotEditSubtitle()`'s create path) — also call
  `pCore->subtitleWidget()->setModel(...)` and
  `TimelineWidget::connectSubtitleModel(true)` afterward so the UI picks
  it up.
- Import an SRT with `SubtitleModel::importSubtitle(path, offsetFrames,
  externalImport=true)`.

## Exporting timeline audio to a file (headless)

Mirrors `SpeechDialog::slotProcessSpeech()`'s render step:

1. `TimelineItemModel::sceneList(rootDir, destPath)` writes the
   project's MLT XML scene to `destPath`.
2. Build an `Mlt::Producer(profile, "xml", scenePath)` from it, walk its
   service chain (`Mlt::Service::producer()` repeatedly) to find the
   `Mlt::Multitrack` whose track count matches the project's track
   count, then `set("hide", 3)` on tracks you want silent (video tracks
   have `hide == 1` by convention — hide those; keep all audio tracks
   for a full mixdown, or hide all-but-one for a single track).
3. Render with an `Mlt::Consumer(profile, "avformat", outPath)`,
   `set("properties", "WAV")`, `producer.set_in_and_out(inFrame,
   outFrame)`, `consumer.connect(producer)`, `consumer.run()`.

This render is **synchronous on the calling thread** (a known,
acknowledged limitation in Kdenlive's own code too — audio-only, so fast
relative to project length, but not truly non-blocking). Needs
`#include "mlt++/MltConsumer.h"`, `"mlt++/MltProfile.h"`,
`"mlt++/MltTractor.h"` — `Mlt::Producer`/`Service`/`Multitrack` resolve
transitively from those.

## The Python plugin install state machine (`AbstractPythonInterface`)

This is the one that cost the most time — traced the hard way instead of
guessed at, twice, before landing on the right call. Read this before
touching *any* optional Python-backed feature (speech-to-text, SAM,
seamless translation — anything under `src/pythoninterfaces/`).

**State**: `InstallStatus { Unknown, NotInstalled, Installed, InProgress,
MissingDependencies, Broken }`, cached in `m_installStatus`. Starts
`Unknown` until something actually probes the venv — don't trust
`status()` without refreshing first.

**The refresh call that's actually correct**: `checkSetup(requestInstall,
&newInstall)`. It resolves `venvPythonExecs()` and, if both python and
pip resolve, sets status to `Installed` — **in either bootstrapped-venv
or `speech_system_python` mode**. `checkVenv()`'s own
`speech_system_python` fast path (`if (useSystemPython()) return true;`)
returns success **without ever updating the cached status** — call
`checkVenv()` directly for a refresh and a system-Python setup reports
`Unknown` forever.

**The real "Install" trigger, mirroring Kdenlive's own button**
(`PythonDependencyMessage` in `abstractpythoninterface.cpp`) — branch on
`status()`, don't call one method unconditionally:

| `status()` | Correct call |
|---|---|
| `Installed` | nothing to do |
| `NotInstalled` / `Broken` / `Unknown` | `checkVenv(false, true)` — creates the venv (blocking `python -m venv` + a smoke-test package install), *then chains into `installMissingDependencies()` itself* once it exists |
| `MissingDependencies` | `installMissingDependencies()` directly — **only valid once a venv already exists** |
| `InProgress` | no-op |

**The trap**: `installMissingDependencies()` alone, called on a system
with no venv, looks like it should work (it pops a confirmation dialog,
sets status to `InProgress`) but silently does nothing. It calls a
private helper (`runPackageScript`) whose `forceInstall` parameter
defaults to `false`; with no venv, that hits `checkVenv(false, false)` →
`if (!forceInstall) return false;` and gives up before ever reaching
`setupVenv()`. No venv gets created, no signal fires, no error — it just
silently reports `{"started": true}` forever. The exact tell in the
terminal log: `"=== CHECKING SETUP...NO!!!"`.

**Bypassing this entirely** (recommended when Kdenlive's own bootstrap
is being unreliable, e.g. it segfaulted mid-install once in testing):
Kdenlive already supports pointing it at a Python environment prepared
outside its control —
`KdenliveSettings::speech_system_python` (bool) +
`speech_system_python_path` (path to a `python3` with a sibling `pip3`),
KCFG group `[speech]` in `kdenliverc`. Create a normal venv in a
terminal, `pip install -r data/scripts/whisper/requirements-whisper.txt`
(the exact dependency list Kdenlive's own Whisper feature uses — also
sets `torch`, `openai-whisper`, `srt` and conditionally `triton`), write
`speech_system_python=true` / `speech_system_python_path=<path>` into
`~/.var/app/org.kde.kdenlive/config/kdenliverc` under `[speech]`. No
GUI, no blocking dialog, no background thread inside Kdenlive at all —
the whole install runs as a normal, observable terminal process.

**Model downloads** (once dependencies are ready): the dialog-based
`installNewModel()` pops its own separate window — for a headless
trigger use `AbstractPythonInterface::runConcurrentScript("whisper/
whisperquery.py", {"task=download", "model=<name>"}, /*feedback=*/true)`
directly instead, same as `WhisperDownload`'s dialog does internally.
Standard OpenAI Whisper model names apply (`tiny`, `base`, `small`,
`medium`, `turbo`, `large-v3`, ...); `turbo` is Kdenlive's own UI
default.

## Flatpak build system

- The exact `flatpak-builder` invocation (flags, and module order in
  `packaging/flatpak/org.kde.kdenlive-dependencies.json`) is load-bearing
  for the build cache — changing a flag, or inserting a module mid-list,
  invalidates everything *after* it and forces a full dependency
  rebuild. New runtime-only modules (a bundled plugin `.so`, say) go at
  the **end** of the dependency list. See `CLAUDE.md` for the pinned
  invocation.
- Debug symbols for a coredump aren't in the base flatpak: `flatpak
  install --user kdenlive-origin org.kde.kdenlive.Debug//master` pulls
  them (~750MB). `coredumpctl gdb <pid>` then resolves real function
  names instead of raw addresses — still needs `set solib-search-path`
  pointed at the flatpak runtime's own library tree to resolve
  everything (host libraries have mismatched build-ids), which wasn't
  fully chased down yet.
- `coredumpctl list` is worth checking before assuming a crash is new —
  this exact nightly build had pre-existing crashes logged from days
  before any vibecut testing, suggesting some baseline instability in
  Kdenlive's own master branch independent of anything we've added.
