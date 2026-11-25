# AMETHYST

A small Windows DAW built for recording vocals. It hosts VST3 effects and saves channel presets. It is written in C++ on JUCE 8.

## Build

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

The app is built to `build\VocalDAW_artefacts\Release\AMETHYST 2026.exe`.

## macOS (13 Ventura and later)

The same code builds a universal Mac app that runs natively on both Apple Silicon and Intel Macs. On macOS it hosts **VST3 and Audio Units**, and it keeps your sign-in in the **Keychain**. A Mac build has to be compiled on a Mac, and there are two ways to do that:

- **GitHub (no Mac needed):** push this folder to a GitHub repo and add the secrets `SUPABASE_URL` and `SUPABASE_ANON_KEY` in the repo settings. `.github/workflows/macos.yml` then builds on a cloud Mac, runs `--selftest`, and uploads `AMETHYST-macOS.dmg` under the run's **Artifacts**. You can also start it by hand from the Actions tab ("Run workflow").
- **On a Mac:** install Xcode and `brew install cmake`, then run `./build-mac.sh`.

The first time AMETHYST opens, macOS asks for microphone access, and may ask about the Documents folder too. Allow both.

The app is ad-hoc signed, not notarised. Because of that, the first time you open a downloaded copy you need to **right-click → Open** (or use System Settings → Privacy & Security → Open Anyway). Distributing it to other people without that warning needs an Apple Developer ID ($99/year) and notarisation, which can be added to the workflow later.

## First run

1. **Audio**: choose your interface. ASIO or WASAPI with a small buffer keeps latency low. Then enable the inputs your mic uses.
2. **Plug-ins**: click **Options â†’ Scan for new or updated VST3 plug-ins** (the default folder is `C:\Program Files\Common Files\VST3`).
3. If no input signal arrives, turn on Windows **Settings â†’ Privacy â†’ Microphone â†’ Let desktop apps access your microphone**.

## Recording vocals

- Pick the track's input (mono `In 1`, or a stereo pair).
- **R** arms the track. **I** turns on input monitoring, so you hear yourself through the insert chain.
- Press **Rec** (or the `R` key) to record. Press **Stop** to end the take, and Stop again to return to the start.
- Takes are saved as 24-bit WAV files in `Documents\AMETHYST\Takes`. Each take is moved back by the device's reported round-trip latency so it lines up with the backing track. A new take is added on top of any older ones.
- **Click** turns the metronome on (it follows the BPM). Plug-ins also get the tempo and transport position, so tempo-synced delays work.
- **Count-in** (next to the metronome; click to cycle off, 1 bar, 2 bars): the transport starts that far before the record point with a click, and only what you sing after the record point is kept.
- **Punch-in:** Ctrl-drag across the ruler to mark a region, which turns **Punch** on, or right-click the ruler. Recording then plays you in from at least a bar before the region (or from the playhead if it's earlier). Only the region is replaced and everything outside stays untouched. Recording stops by itself just after the region. Undo brings the old take back.
- **Autosave:** every 2 minutes AMETHYST saves a copy of your session to `Documents\AMETHYST\Autosave`. If it didn't close properly (a crash or a power cut), it offers to restore that copy the next time it starts, and Save then writes back to your original project.

## Tracks and editing

**Track list.** New sessions start with 8 slim track rows. Each row has:
- the track name (double-click to rename)
- a red **record-arm** light and an **on/off** light (mute)
- a **...** menu with recording input, input monitoring, solo, plug-ins in the mixer, channel presets, overlapping takes, and delete
- small chips that show the current input, MON, SOLO, and the number of FX

Volume and pan are in the **Mixer** (Tab key).

**Tools** (top-left above the tracks):
- **Move (V):** drag a clip to move it. Drag either edge of a clip to trim it.
- **Trim (T):** grab the left half of a clip to trim its start, or the right half to trim its end.
- **Cut (C):** click a clip to split it there. A preview line shows where the cut will go.
- Hold **Shift** while moving, trimming or cutting to snap to beats.

Trimming is non-destructive: drag an edge back out, or right-click a clip → **Restore full length**. Right-click a clip for:
- Split here / Split at playhead
- Trim start or end to playhead
- Delete
- Clip properties

Cut and trimmed edges get tiny automatic fades, so they don't click.

**Navigating.**
- **Ctrl + scroll wheel** zooms in and out around the mouse (Cmd on Mac).
- **Shift + scroll** (or scrolling over the ruler) moves left and right.
- Dragging the playhead along the ruler, or dragging a clip, into the left or right edge scrolls the timeline. It goes faster the further in you push.

**Magnet snap and grid.** The lanes show alternating bar shading, bar lines, beat lines and (when you zoom in) the snap lines.
- The **magnet** button next to the tools picks the snap size: none, 1/6 to 1/2 step, step, 1/6 to 1/2 beat, beat or bar.
- Moving, trimming, cutting and clicking to place the playhead all snap to it.
- Hold **Shift** to drag freely.

**Clip right-click menu.** It has four sections:
- **Clip:** Preview (plays only that clip), Muted, Rename, Colour (10 colours or random), Clip properties, Duplicate, Send to track, Show mixer channel, Show file, Delete.
- **Edit:** Split here / at playhead, Trim to playhead, Restore full length, Move to playhead, **Chop** (every bar, beat, half beat or snap step), **Split at silences**, **Remove silence**.
- **Sound:** Reverse, Normalize, Pitch (±12 semitones, keeps the length), **Detect tempo**, **Fit to project tempo**.
- **Fades:** fade in, fade out, reset.

All of these can be undone.

**Knobs and faders.** Right-click any knob or fader for Reset, Copy value, Paste value and Type in value.

**Tempo.** Right-click the BPM display for:
- **Tap tempo:** click the display on each beat.
- BPM presets from 80 to 160.
- Half or double the tempo, round to a whole number, or type in a value.

**Take lanes.** A take that auto-moves to another track keeps playing through the **original track's mixer channel** (its plug-ins, volume and pan), so it never changes level. The track row shows "→ Track 1". A take lane has no mixer strip of its own. To change this, use the row's **...** → **Play through channel**.

**Undo.** Press **Ctrl+Z** to undo and **Ctrl+Y** (or Ctrl+Shift+Z) to redo; on Mac use Cmd. This covers:
- moving, trimming, cutting and deleting clips
- enabling or disabling clips
- recording a take
- importing audio

Up to 200 steps are kept. Undoing a delete brings the clip back with its trims. Mixer and plug-in changes aren't part of undo yet.

**Waveforms** draw a thin centre line through silent stretches, so quiet parts show a line instead of an empty gap.

**Where new takes go.** If a new take would land on top of an existing take, AMETHYST moves it to the next free track below. It skips armed and muted tracks, and makes a new track if none is free. If that track is completely empty, it also gets the original track's plug-ins, volume and pan, so the take sounds the same. To keep takes on the track you recorded on, open the track's **...** → **Takes** → **Keep new takes on this track**.

**Recording over old takes.** Earlier takes on a track keep playing while you record a new one, so you can hear what you're doubling. Afterwards, overlapping takes are **layered** (you hear all of them) by default. Switch a track to **Newest take on top** from its **...** menu, or by right-clicking its lane.

## Accounts and AMETHYST Pro

The 10 built-in AMETHYST effects are **Pro** features. Everything else is free: recording, mixing, VST3s, clip effects, presets and export.

- The **Sign in** button (top right) opens the account window. From there you can sign in, create an account, reset your password, refresh your plan, or sign out.
- When signed in, the button shows **FREE** or **PRO**.
- **Profile pictures (Pro only):** in the account window, **Change picture...** lets you pick a PNG or JPG. It's cropped to a circle, uploaded to your account, and shown in the toolbar. Free accounts see their initial instead.
- Free users see the built-in effects with a **PRO** padlock badge. Adding one opens the upgrade prompt.
- If a project or channel preset uses a Pro effect, it opens without that effect and shows a message saying so.
- The session is saved encrypted with Windows DPAPI, so it only works for your Windows user. Pro keeps working offline for 7 days after the last successful check. The app re-checks your plan every 30 minutes.

### Setting up Supabase

1. Create a Supabase project. Under **Authentication → Providers**, make sure Email is enabled. Leaving "Confirm email" on is fine, because the app handles it.
2. Open the **SQL editor** and run `supabase/migrations/20261124000000_entitlements.sql`. Then run `supabase/migrations/20261124010000_avatars.sql`, which sets up **profile pictures**: a private `avatars` storage bucket where only active Pro users can upload, enforced by the database. Last, run `supabase/migrations/20261124020000_ultra_pro.sql`, which adds the **Ultra Pro** tier (an `is_ultra` flag; Ultra includes everything in Pro). The app still works if this last one hasn't been run yet; nobody is Ultra until it is. It creates an `entitlements` table with row-level security: users can read their own row but can't change it. It also adds a trigger that gives every new user a free row.
3. Copy `config/supabase.example.json` to `config/supabase.json` (this file is gitignored). Fill in your **Project URL** and **anon / publishable key** from Project Settings → API. Never use the `service_role` key here.
4. Rebuild. CMake prints `AMETHYST: Supabase configured`.

**Until payments are set up**, you can give someone Pro by hand in the SQL editor:

```sql
update public.entitlements set is_pro = true, plan = 'pro_manual', pro_until = null
where user_id = (select id from auth.users where email = 'someone@example.com');
```

Later, a payment webhook (for example, a Supabase Edge Function using the service-role key) only has to set `is_pro`, `plan` and `pro_until`. The app needs no changes.

**Note:** the Pro check runs inside the app, so a determined person could patch it out. That's normal for desktop apps. Stronger protection, such as server-signed licences, can come with payments.

To test the account client without the real server: `"AMETHYST 2026.exe" --account-test <url> <anon-key>` runs sign-in, refresh, entitlement, sign-up and reset against whatever server you point it at.

### Signed licenses (anti-tamper)

Paid plans are checked with **signed licenses**. The `license` Edge Function reads the user's row in `entitlements` and returns a license ("this account, this plan, works offline until...") signed with a private key that only Supabase holds. The app checks the signature with the public key built into it (`Source/LicensePublicKey.h`). This means:
- **Editing the saved account data doesn't work.** A changed plan, date or account breaks the signature, and the license is ignored.
- **Offline for 14 days.** After that, AMETHYST asks to go online once. A subscription with an end date (`pro_until`) stops on time, even offline.
- **Clock protection.** AMETHYST remembers the latest time it has seen (and the server's time). If the PC's clock is set back more than 10 minutes, paid features pause until the clock is fixed and the plan is re-checked. Normal clock drift is fine.

**Setup (once):**
1. The key pair is made by `node tools/make-license-keys.js`. It writes `config/license_private.pem` (private: git-ignored, never share it, and **keep a backup**) and the public key into the app. Running it again makes a new pair, so only do that if the private key leaks.
2. Supabase dashboard > **Edge Functions** > Deploy a new function named `license` with the code in `supabase/functions/license/index.ts` (or `supabase functions deploy license`). Leave "Verify JWT" on.
3. Edge Functions > **Secrets**: add `LICENSE_PRIVATE_KEY` with the whole contents of `config/license_private.pem`, including the BEGIN/END lines.
4. Ship the new build (installer + update).

Until the function is deployed, AMETHYST falls back to checking the `entitlements` table live. That fallback counts only while the app is open and is never saved, so a tampered cache can't use it, but there's no offline use either. Dev check: `"AMETHYST 2026.exe" --license-verify <token>` tells you whether a license verifies against the built-in key.

## AMETHYST TUNE (Ultra Pro)

Real-time vocal pitch correction, available to **Ultra Pro** accounts. It shows an **ULTRA** badge in the plug-in list.

The window has:
- **Key column:** pick the key. **LEARN** listens to about 8 seconds of singing and sets the key and scale for you.
- **Dial:** the live waveform, plus the note you're singing and how many cents it's being corrected.
- **Retune speed:** how fast notes are pulled into tune. 0 ms gives a hard, robotic snap; slower settings sound more natural.
- **Humanize:** lets small natural wobbles like vibrato through.
- **Scale:** chromatic, major, minor, harmonic minor, pentatonics or blues.
- **Scale lock:** freezes the key, so Learn and piano clicks can't change it.
- **Tracking:** how forgiving pitch detection is with breathy or noisy singing.
- **Formant:** Auto counters the brightness change when pitch is moved.
- **Throat:** gives a deeper, fuller character.
- **Piano:** click keys to choose exactly which notes are allowed (a custom scale). Notes you're singing light up.
- **Input type:** soprano, alto/tenor, low male or instrument.
- **Mix**, plus a header with presets, undo/redo, reference pitch (432–444 Hz) and bypass.

To give someone Ultra Pro by hand, run this in the SQL editor:

```sql
update public.entitlements set is_ultra = true, is_pro = true, plan = 'ultra_manual', pro_until = null
where user_id = (select id from auth.users where email = 'someone@example.com');
```

## Built-in effects

AMETHYST ships with 21 effects of its own, including **Tape Stop** (automate its Play switch for wind-downs) and **Polish** (one-click mastering). They appear at the top of the plug-in browser, marked with the gem icon. Each has:
- factory presets, with **<** and **>** to step through them
- **Save preset...**, which saves to `Documents\AMETHYST\Plugin Presets`
- Randomize and Reset
- **IN / OUT meters** and **A / B compare** in the header: two complete settings slots, and **A>B** copies the current one across

| Effect | What it is |
|---|---|
| **Facet EQ** | 7-band parametric EQ. Shows the output spectrum, with the input spectrum as a faint line behind it. Low/high cuts can be 12, 24 or 48 dB per octave. Drag a node to change frequency and gain, scroll on it for Q, double-click it to flatten. **Alt-drag** a node, or right-click it and choose **Listen**, to hear just that band. |
| **Prism Dynamics** | Compressor or limiter that smooths the gain reduction itself, so fast settings stay clean. Peak or RMS detection, soft knee and **Auto gain**. Three characters: **Clean**, **Smooth** (release slows down after long compression, for natural levelling) and **Vintage** (adds warmth as it compresses). The **noise gate** has hysteresis (opens at the threshold, closes 6 dB lower), a **Hold** time, a 1.5 ms fade-in and a release that glides down. Its detector ignores rumble below 80 Hz, so it doesn't chatter or click. Also has saturation, a ceiling, parallel mix, **sidechain** from any track and a **3-band mode**. |
| **Geode Reverb** | An 8-line feedback-delay-network reverb with **Room, Hall, Plate and Chamber** styles. Decay is set in real seconds (0.1 to 20 s; the self-test measures it). Bass multiplier, crossover and high damping work like FL's Reeverb 2. Also has modulation, **Duck** (the tail dips while you sing), low/high cut, width and dry/early/wet. |
| **Crystal Echo** | Delay with four styles: **Clean, Tape** (wow/flutter and saturation), **Lo-fi** (crushed and band-limited) and **Diffuse** (smeared repeats). Also has Normal, Invert or Ping-pong feedback, a feedback low and high cut, drive, **Duck** (echoes dip while you sing and bloom in the gaps), tempo-sync divisions and stereo offset. |
| **Prism Mod** | Flanger, chorus (3 voices) or vibrato, with stereo spread and feedback. |
| **Lustre** | One-knob enhancer: three-band compression and saturation, with flavours A–D. |
| **Silk De-Esser** | Tames S and T sounds. Split or wide mode, plus a Listen switch to hear what it detects. |
| **Ember** | Tape, tube, fuzz or bit-crush saturation, with tone and mix. Shows the transfer curve. |
| **Twin Voice** | Vocal doubler: two detuned voices with timing variation, spread across the stereo field. |
| **Shift** | Pitch shifter (±12 semitones plus fine-tune) for harmonies and octave effects. |
| **Vocal Rider** (Pro) | Rides the vocal level towards a target before any compressor, like an engineer on the fader. Holds still between phrases so it never pumps up the room. Shows the riding as a graph. |
| **Denoiser** (Ultra Pro) | Spectral noise reduction for hiss, hum and room noise. Turn on **Learn** during a pause to capture the noise (saved with the project), or leave **Adaptive** on to follow the noise floor by itself. Reports its latency, so delay compensation keeps it in time. |
| **Harmonizer** (Ultra Pro) | Up to two extra voices that follow your singing in key (3rd, 4th, 5th, 6th, octave, up or down), or fixed intervals. Humanize adds drift and lag so it sounds like real singers; Spread pans them apart. |
| **Stereo Imager** (Pro) | Width (0-200%), mono bass below a frequency, balance, with a vectorscope and correlation meter. |
| **Transient Shaper** (Pro) | More or less attack and sustain, independent of level: punchier consonants, softer plosives, drier rooms. |
| **Rhythm Gate** (Pro) | Tempo-synced 16-step volume patterns (trance gate, offbeat pump, stutter, chop) with depth, smoothing and mix. |
| **Auto Filter** (Pro) | Resonant low/high/band-pass swept by a tempo-synced or free LFO (4 shapes) and an envelope follower (wah). |
| **Loudness Meter** (free) | LUFS momentary / short-term / integrated (ITU BS.1770 with gating), true peak, a short-term history and target lines for Spotify, Apple Music, club and broadcast. |

Running `"AMETHYST 2026.exe" --selftest` checks every factory preset of every effect, and the clip-effects renderer, without opening the UI.

## Mixer and plug-in slots (VST3)

Click **Mixer** (or press `Tab`) to open the mixer. It has three parts:
- **Left:** a searchable list of your scanned effect plug-ins.
- **Middle:** a vertical strip for the Master channel and for each track. Each strip has a name, a record-arm LED, pan, mute/solo, a fader and meters. Click a strip to select it.
- **Right:** "Mixer - <channel>", showing the input selector and **10 plug-in slots** for the selected channel. The Master channel has its own slots too.

To put a plug-in in a slot, you can:
- drag it from the list onto a slot,
- double-click it in the list (it goes into the first free slot),
- drag a `.vst3` file from Explorer onto a slot, or
- click an empty slot and pick from the menu.

Each slot also lets you:
- click it to open the plug-in's editor
- use the **LED** to switch the slot on or off
- use the **knob** to set the wet/dry mix
- drag it onto another slot to swap the two
- right-click it to replace, move or remove the plug-in

**FX** on a track's header jumps to that track in the mixer.

## Look ("Amethyst" glass theme)

The interface uses a custom dark-purple "liquid glass" style throughout:
- frosted, see-through panels
- glossy knobs and faders
- violet-to-pink glow on anything that's switched on

The toolbar has three zones:
- **Left:** return-to-start, Stop, Play and Record; then the Metronome, **Count-in** (click to cycle; the badge shows the bars) and **Punch**.
- **Centre:** one glass display with the **tempo** (drag, scroll or double-click to type) and the **position** (click to switch between bar:beat:tick and min:sec.ms). The CPU meter joins it when the window is wide enough.
- **Right:**
  - **File:** new, open, open recent, save, save as, import FL Studio, welcome screen.
  - **Export** and the **Mixer** toggle (Tab).
  - **Tools** and **Settings** (gear): audio device, plug-in manager, colour theme, background picture, welcome screen at start.
  - **Master** volume and meter.
  - Your **account** badge: a gem for Pro, and a crown with a slow shimmer for Ultra Pro.

**Welcome screen:** start an empty project, a **Vocal session** (Beat, Vocals, Adlibs and Harmonies; with Pro the Vocals track comes armed with EQ, compressor, de-esser, echo and plate reverb, plus AMETHYST TUNE with Ultra Pro), **Mix a beat** (Polish on the Master), or import an FL project. It also lists your recent projects and what's new. Turn it off from Settings.

**Colour themes** (Settings > Colour theme): Amethyst for everyone; **Rose Quartz, Sapphire and Emerald** with Pro; **Citrine and Obsidian** with Ultra Pro. Everything recolours: panels, knobs, glows and effect windows.

Track rows stay calm: the **...** button appears when you hover a row.

## Clip properties (double-click a clip)

Each clip has its own non-destructive settings. The original file is never changed: the settings are applied in the background, and the waveform updates to show the result.
- **Header:** clip on/off, Pan, Vol, Pitch (Â±24 semitones).
- **File:** replace the audio, show it in Explorer, or delete the clip.
- **Time stretching:**
  - *Resample* mode: changing speed also changes pitch, like tape.
  - *Stretch* mode: speed and pitch change independently. Fine-tune knob in cents.
- **Shape page:**
  - Switches: Remove DC offset, Normalize, Reverse, Invert polarity, Stereo fade, Swap stereo.
  - Knobs: Start, Length, Fade in, Fade out, Trim (cuts silence below a threshold).
- **Color page:** Boost, Tone (darkâ†”bright), Ring-mod mix and frequency, low-pass Cut and Res, Soft clip, Reverb (A = room, B = hall), tempo-synced ping-pong Delay, and Pogo (a pitch glide across the clip).
- **Render:** declicking (tiny fades at the clip edges), source/result info, and Reset all.

Clips with effects show `*fx` on their label. All of these settings are saved in the project.

## Importing audio (drag and drop)

You can drag WAV, MP3, FLAC, AIFF, OGG or WMA files from Explorer onto the arrangement:
- **On a track lane:** the clip lands on that track, at the point where you dropped it.
- **Below the last track:** a new track is created, named after the file.
- **Several files at once:** the first goes where you dropped it, and each of the others gets its own track.
- **Into the mixer:** the file goes onto the selected track at the playhead.

Once a clip is on a track:
- **Drag** it to move it. Hold Shift while dragging to snap to beats.
- **Right-click** it for Delete, Show in Explorer, or Move to playhead.
- A track can hold any number of clips. Where clips overlap, the newest one plays, so a re-recorded take covers the old one.

## Automation clips

An automation clip is its own row in the track list. It holds an envelope that moves one control over time.

- **Create one** in either of two ways:
  - Right-click any knob in an AMETHYST effect and choose **Create automation clip**.
  - Open a track's **...** menu and choose **Create automation clip**, then Volume, Pan, or any parameter of a plug-in in its slots. VST3s are included.
  - For the Master's volume or pan, use **Tools > Automate Master**.
- **Edit it:**
  - Click the lane to add a point. Points snap to the grid; hold Shift for free placement.
  - Drag a point to move it.
  - Right-click or double-click a point to delete it.
- **Shapes:** the clip's **...** menu has ready-made shapes: flatten, ramp up, ramp down, and "off for the last beat of every bar", which suits Tape Stop drops. It also switches the automation on or off, or deletes it.
- Automation is saved with the project. Point edits aren't undoable yet.

## Tools menu

- **Nightcore Maker (Ultra Pro):** load or drop a song and pick Nightcore, Slowed + Reverb, or Custom. Set speed (pitch moves with it), reverb, bass boost and volume. Preview it, then export WAV/MP3 or add it to the project as a new track.
- **Find song tempo & key:** analyses all the audio in the project and offers to set the tempo. Single clips have **Detect tempo...** and **Detect key...** in their right-click menu. Detect key can set AMETHYST TUNE on that channel to the key it finds.
- **Master presets (Pro):** puts **Polish**, the one-click mastering effect, last on the Master. It has 8 styles (Balanced, Warm, Bright, Tape, Natural, Wide, Cinematic, Punch), intensity, and low/mid/high tone.

## Importing FL Studio projects (.flp)

Open (Ctrl+O) also accepts FL Studio projects, from FL 10 up to FL 2026. AMETHYST builds a new session and shows a report of what came across:

- **Tempo**, and **playlist audio clips** at their positions, with their start and length trims and mute state.
- **Mixer inserts** that have audio, effects or a name each become a channel, keeping the insert name, volume and pan. If one insert is fed by several playlist tracks, the extra tracks become take lanes that play through that channel. Master insert effects go on the Master.
- **FL native effects** are swapped for the closest AMETHYST effect. For the most used ones, the settings come across too. The report says how closely:
  - **Parametric EQ 2:** band types, frequencies and gains are exact. Q is approximate.
  - **Reeverb 2:** low/high cut, predelay, size, diffusion, dry, early and wet are exact. Decay time and damping are converted to Geode's scale.
  - **Compressor:** threshold, ratio, gain, attack and release are exact. Knee is taken from the compressor type.
  - **Limiter:** gain, ceiling and the noise gate come across. Gate release is approximate.
  - **Soundgoodizer:** mode A–D and amount are exact.
  - **Pitcher:** the key / scale notes are exact (matched to a Tune key and scale, or Tune's Custom notes). Correction speed is converted to Tune's retune time.
  - **Newtone:** its edits are note-by-note on one recording, so Tune is set to natural correction instead.
  - **Delay 2:** time, feedback, ping-pong / invert mode, feedback cutoff and levels come across. Stereo offset is approximate.
  - **Fruity Filter:** becomes a low cut, high cut or band pass on Facet EQ at the same cutoff, with x2 as a steeper slope.
  - **Blood Overdrive, Fast Dist, Soft Clipper:** drive, tone and output are converted to Ember.
  - **Stereo Enhancer:** at rest it stays silent. Widening is approximated with a tight double on Twin Voice.
  - **Multiband Compressor:** becomes 3-band Prism. Crossovers, master and band output gain, threshold, ratio and attack are converted using scales measured by rendering FL's own plug-in. Prism has a single threshold, so the band doing the most work sets it, and each band's strength is matched to its gain reduction at a typical vocal level. Bypassed bands get no compression. Muted bands can't be reproduced; the report says so.
  - **Maximus:** becomes 3-band Prism in limit mode with the same crossovers, drive (pre/post gains) and output ceiling, also using measured scales. Maximus's hand-drawn compression curves are approximated by limiting.
  - **Delay 3 and Flangus:** approximated.
  - Other effects load at default settings.

  | FL Studio | AMETHYST |
  |---|---|
  | Fruity Reeverb 2 / Reeverb / Convolver | Geode Reverb |
  | Fruity Parametric EQ 2 / 7 Band EQ / Filter / Love Philter | Facet EQ |
  | Fruity Limiter / Compressor / Multiband / Maximus / Emphasizer | Prism Dynamics |
  | Fruity Delay 2 / 3 / Delay Bank | Crystal Echo |
  | Fruity Flanger / Chorus / Flangus / Phaser | Prism Mod |
  | Soundgoodizer | Lustre |
  | Fruity Stereo Enhancer / Stereo Shaper | Twin Voice |
  | Pitcher / Newtone | AMETHYST TUNE (Ultra Pro) |
  | Pitch Shifter | Shift |
  | Blood Overdrive / Fast Dist / WaveShaper / Soft Clipper | Ember |

- **VST plug-ins** (Fruity Wrapper) load by name if they're installed and scanned, or straight from the .vst3 file FL used. VST3 settings are restored from the project, for example your Auto-Tune key and scale.
- **Not imported:** patterns, MIDI and instruments, automation, and routing from one insert to another. Each is listed in the report.
- Sample paths are resolved, including `%FLStudioFactoryData%`. For moved projects, AMETHYST also searches by file name next to the .flp and in its `Audio` folder.
- An imported project isn't linked to the .flp. Save it as a new `.amethyst` file.

### FL Studio presets (.fst)

Most vocal presets are saved from FL as a **mixer track state** (`.fst`). To use one, drop it on a track, the mixer, or an effect slot. Or open a channel's **Presets** menu and choose **Load FL Studio preset (.fst)...**.

- A mixer track state replaces the channel's effects, the same way it does in FL, using the translations above. It also sets the fader and pan, and each slot's on/off and mix.
- A single effect preset (for example a Pitcher or Reeverb 2 preset) goes into the channel's first free slot.
- Dropping a preset on empty space makes a new channel named after the preset. Presets can also go on the Master.
- A report lists what came across and what didn't, just like project import.

Dev checks: `"AMETHYST 2026.exe" --snapshot <pluginId|nightcore> out.png` renders an editor offscreen. Test modes can run while AMETHYST is open. `"AMETHYST 2026.exe" --flp-scan <folder>` reads every .flp below a folder (reader only). `"AMETHYST 2026.exe" --flp-test <file.flp> ...` runs a full import and prints the report. `"AMETHYST 2026.exe" --fst-test <file.fst|folder> ...` loads each FL preset onto a fresh channel and prints the report plus every parameter that changed.

## Channel presets

On any track, **Preset â†’ Save channel preset...** saves the following to a `.vchp` file in `Documents\AMETHYST\Channel Presets`:
- the fader and pan settings
- the whole insert chain: which plug-ins, the order, the bypass state, and each plug-in's full internal state

Saved presets appear in the same menu, so you can load one onto any track with a click.

## Projects and export

- **Save/Open** work with `.amethyst` session files (older `.vdaw` projects still open), which hold the tracks, routing, channel strips and takes. Shift-click Save for Save As.
- **Export** opens a format menu:
  - **Wave** (24-bit). Shortcut: Ctrl+R.
  - **MP3** (320 kbps). Shortcut: Shift+Ctrl+R.
  - **OGG**.
  - **FLAC** (24-bit).
  - **M4A** (AAC, 192 kbps).
  - **Last exported format**.

  The whole song, plus a 3-second tail for reverbs, renders offline, much faster than real time, with a progress bar and a Cancel button. The metronome is never included. MP3 and M4A use the encoders built into the operating system. Windows has both; macOS has M4A only, so MP3 is greyed out on Mac.

Keys: `Space` play/pause Â· `R` record Â· `Home` return to start Â· `Ctrl+S` save Â· `Ctrl+O` open

## New in AMETHYST 2026 (second wave)

**Keyboard and workflow**
- **Command palette (Ctrl+K):** type a few letters of anything (export stems, mono check, add marker...) and press Enter. Every command shows its shortcut.
- **Shortcut sheet (F1):** all keyboard shortcuts on one screen.
- **Toasts:** short messages at the bottom confirm saves, captures, bounces and more, without a dialog to click away.
- **Markers:** **M** drops a marker at the playhead (named Intro, Verse, Pre-hook, Hook... in order). **,** and **.** jump to the previous / next one. Right-click the ruler to rename or delete. Saved with the project.
- **Loop playback:** **L** loops 4 bars from the playhead's bar (or turns the loop on and off). **Alt-drag** on the ruler sets the region; the ruler's right-click menu has "Loop this bar" and "Loop the punch region". The jump back is sample-accurate.
- **Lyrics & notes (Ctrl+L):** a side panel for lyrics and ideas, saved with the project. **Big text** for singing along.
- **Track colours:** the track's **...** menu > Track colour. The colour shows on the row, its clips and its mixer strip.
- **Templates:** File > **Save as template...** keeps the tracks, routing, effects and settings (no audio). File > **New from template** starts from one.
- **Version history:** every save keeps the previous one in a "<project> versions" folder next to it (the 20 newest). File > **Restore earlier version** opens one; saving then replaces the current project.

**Audio engine**
- **Plug-in delay compensation:** every channel is delayed to line up with the one whose plug-ins add the most latency (lookahead limiters, linear-phase EQs, the Denoiser). Exports, takes and retrospective captures are compensated too. The status pill on the ruler shows the delay.
- **Low-latency monitoring** (Settings > Monitoring, on by default): while recording, an armed and monitored track skips latent plug-ins so you hear yourself with no extra delay. The take is unaffected.
- **Monitor modes (Ctrl+M):** stereo, mono check, side (L-R) check, left only, right only. For checking only; exports are never affected.
- **Channel strip extras** (track **...** menu): a **low cut** (rumble filter, 40-150 Hz) and **polarity invert** before the plug-ins.
- **Mixer meters** now have peak-hold ticks and a red clip light per channel (click it to clear).
- **Metronome options** (Settings > Metronome): classic beep, woodblock or hi-hat, volume 25-150%, and accent on/off.
- **Live loudness:** while playing, the status pill shows the mix's short-term LUFS.

**Pro and Ultra Pro perks**
- **Capture last performance (Shift+R, Pro):** AMETHYST keeps the last minute of the armed track's input even when you're not recording. Sang something great while just practising? Press Shift+R and it becomes a take, placed exactly where you sang it if the song was playing.
- **Smart Vocal Chain (Pro):** the track's **...** menu, or Tools. Analyses the vocal (level, muddiness, presence, air, where the S sounds are) and builds a tailored chain: Facet EQ, Prism compressor, Silk De-Esser, a ducked echo and a short plate. A report explains every choice. AMETHYST TUNE stays first if it's there.
- **Bounce in place (Pro):** renders a track through its effects into one new clip. The effects are switched off and the original audio is kept (muted), so you can go back.
- **Stem export (Pro):** Export > Stems writes one 24-bit WAV per track (post-fader, without master effects), numbered in track order.
- **Loudness-matched export (Pro):** Export > Loudness target matches exports to -14 (Spotify / YouTube), -16 (Apple Music), -9 (club) or -19 LUFS (podcast) with a -1 dBTP true-peak ceiling. Every export now reports its loudness and true peak.
- **Reference track A/B (Pro):** Tools > Reference track > Load. Press **Q** while playing to hear the reference at the same point in the song, loudness-matched to your mix, then Q again to hear your mix.
- **Reverse reverb swell (Pro):** right-click a vocal clip > Sound > Reverse reverb swell. Adds a rising reverb lead-in that ends exactly where the phrase starts.
- **Set input level:** the track's **...** menu. Sing your loudest part for 5 seconds and the input gain is set so peaks land around -9 dBFS.
- **New effects:** Vocal Rider, Stereo Imager, Transient Shaper, Rhythm Gate and Auto Filter (Pro); Denoiser and Harmonizer (Ultra Pro); the Loudness Meter is free for everyone.

## New in 1.1 (pro workflow)

- **Settings window (Ctrl+, or gear > Settings...):** everything in one place, in tabs: Audio (device), Recording (count-in, low-latency monitoring, return to start on stop, auto-crossfade, metronome, monitor mode), Appearance (themes, background, startup animation, welcome screen), Files & export, Online (updates, Discord) and Shortcuts.
- **What's new:** after an update, a short card lists the changes once. Reopen it from the command palette, or turn it off in Settings > Appearance.
- **Plug-in crash protection (Windows):** if a third-party plug-in crashes while processing, AMETHYST catches it, bypasses that slot, silences its output and tells you which plug-in it was, so the session and your unsaved work carry on. Save, then remove or replace the plug-in.
- **Take comping: Comp tool (X):** with take lanes playing through one channel, swipe across the part of a take you want. AMETHYST splits the takes at the swipe edges, keeps that part and mutes the other takes there. A click keeps a whole take. Each swipe is one undo step; muted pieces stay in place, so you can change your mind later.
- **Crossfades:** where two clips on a track partly overlap (up to 2 s), they crossfade automatically with an equal-power curve, shown as an X over the overlap. Turn it off in Settings > Recording.
- **Clip gain:** drag the small handle at the top centre of a clip up or down (Shift for fine steps). The waveform follows, and it snaps back to 0 dB near the middle. It's saved with the project and copied with splits and duplicates.
- **Undo for the mixer and plug-ins:** fader, pan, mute/solo, low cut, polarity, sends and every plug-in slot (inserting, removing, bypass, mix and every knob) are now in Ctrl+Z / Ctrl+Y. A change becomes one step once you let go. Automated values are not recorded.
- **Send buses:** in a channel's slot panel, **+ FX bus** adds a reverb bus (plate), a delay bus (1/4-note echo) or an empty bus, with a 20% send from that channel. Up to 4 buses. The send knobs on every channel are post-fader, and buses show "FX" on their mixer strip. The command palette has "Add reverb bus" and "Add delay bus" too.

### The angel (anime icon and Easter eggs)

- **Settings > Appearance > App icon & logo:** choose the **Amethyst gem** or the **Anime angel**. The choice changes the logo everywhere (toolbar, splash, welcome screen, plug-in windows) and the running window's icon. The installer and desktop shortcut keep the gem. The command palette has "Switch app icon" too.
- She's the **Nightcore Maker's** icon, in its window and in the Tools menu.
- Now and then she peeks in from the bottom-right corner. Some of the triggers: clicking the logo quickly a few times, the master clipping, finishing an export, late-night sessions, a certain famous cheat code, and "Summon the angel" in the palette. Turn the surprise ones off with **Angel Easter eggs** in Settings > Appearance.
- The art lives in `Resources/mascot*.png` (built in as binary data); replace those files to change her.

Dev checks: `--selftest` now also measures the new effects (EBU loudness reference tone, harmonizer intervals, denoiser reduction...) and the engine features (delay compensation, loop, monitor modes, channel render, reference, retro capture, metronome, low cut, low-latency monitoring). `--ui-test [folder]` drives the new UI through a hidden window and writes snapshots.

## Copy, paste and quick delete

- **Select:** click a clip (it gets a white outline). **Ctrl-click** adds or removes clips. **Ctrl-drag** across a track highlights part of it - for example just the chorus of a long take. **Esc** clears the selection.
- **Ctrl+C** copies the selected clips or the highlighted part. **Ctrl+V** pastes at the playhead on the track you last clicked. If the playhead is still inside what you copied, it pastes straight after it. The pasted audio is selected, so you can drag it anywhere.
- **Ctrl+X** cuts (a highlighted part leaves a gap in the take), **Ctrl+D** duplicates right after, **Delete** removes the selection. All of it can be undone.
- Dragging one of several selected clips moves them all together.
- **Hold the right mouse button** on a clip to delete it, or keep holding and drag across clips (even on other tracks) to delete everything you touch. A quick right-click still opens the clip menu.

## Tempo finder

Clip > Detect tempo and Tools > Find song tempo & key analyse the onsets in 5 ms steps across frequency bands, find the beat period, then fine-tune it in 0.01 BPM steps across the whole clip. Whole-number tempos come back exact, and in-between ones like 87.5 too. Some beats can be counted at half or double speed (a 140 trap beat with a half-time snare also feels like 70), so the result offers **Set**, **Half** and **Double**. The tempo is set exactly, no longer rounded.

## Audio files (Settings > Audio files)

- **WAV / AIFF / FLAC play untouched** (on by default): lossless files play exactly as recorded. No automatic fades at the file's own start and end, so a drum hit on the very first sample keeps its full punch. Only an edge you cut or trim gets the tiny anti-click fade.
- **Trim MP3 encoder padding** (on by default): MP3s carry about 25 ms of silence the encoder added. AMETHYST reads the gapless header (LAME / FFmpeg) and removes exactly that padding when importing, so beats line up on the grid. MP3s without that header are left as they are. This applies to MP3s imported from now on; projects saved earlier that use MP3s may play them about 25 ms earlier.
- Opening an audio file "with AMETHYST" from Explorer adds it as a new track; it's never treated as a project file.

## Known limits

- You can move and delete clips, but you can't trim or split them yet, and there's no undo.
- Imported files are referenced where they are, not copied into the project.
- Plug-ins are scanned in-process. A plug-in that crashes during a scan gets blacklisted the next time you scan.
- Takes recorded at a different sample rate are resampled when they load. Changing the device rate while a session is open does not reload clips that are already loaded.

## Discord status

AMETHYST 2026 can show what you're doing on your Discord profile: "Recording vocals", "Mixing", "Writing a song", "Making a nightcore edit" and so on. It also shows how long you've been at it, the AMETHYST logo, and a Pro or Ultra Pro badge. It talks straight to the Discord app on your computer, with no extra download.

- **Settings (gear) > Discord status**: turn it on or off, choose whether to show the project name (off by default, so only "6 tracks - 140 BPM" shows), and see whether it's connected.
- **One-time setup**:
  1. Create an application at https://discord.com/developers/applications. Its name, e.g. "AMETHYST 2026", is what Discord shows as "Playing ...".
  2. Under **Rich Presence > Art Assets**, upload `Resources/icon.png` with the key `amethyst`, plus optional `pro` / `ultra` images.
  3. Paste the **Application ID** into **Settings > Discord status > Set application ID**, or build it in with `-DAMETHYST_DISCORD_APP_ID=...`.

## Windows installer and auto-updates

**Installer:** `dist\AMETHYST-2026-Setup-<version>.exe`, built from `installer\AMETHYST2026.iss` (Inno Setup 6).
- Installs for the current user with no admin prompt, to `%LOCALAPPDATA%\Programs\AMETHYST 2026`.
- Adds a Start-menu shortcut, an optional desktop shortcut and an uninstaller in Windows Settings.
- Double-clicking a `.amethyst` file opens it. If AMETHYST is already open, the project opens in that window.
- The app links the C++ runtime statically, so nothing else needs installing.

**Auto-updates:**
- About 8 seconds after starting, and every 6 hours while it's open, AMETHYST reads `latest.json` from the public `releases` bucket of your Supabase project.
- If a newer version is there, it offers **Update now / Later / Skip this version**.
- **Update now:**
  - saves the project (or keeps the autosave)
  - downloads the installer with a progress bar
  - checks its SHA-256
  - runs it silently; the installer closes AMETHYST, updates it and starts it again, and AMETHYST offers to pick up where you left off.
- Installers are only accepted from that bucket, over https.
- **Settings > Check for updates now** and **Check for updates automatically** control it. The menu also shows the installed version.

**Publishing a release:**
1. One time: run `supabase/migrations/20261125000000_releases.sql` in the Supabase SQL editor. It creates the public, read-only `releases` bucket.
2. Bump `project(VocalDAW VERSION x.y.z)` in `CMakeLists.txt`.
3. Run `.\tools\make-release.ps1 -Notes "What's new"`. It builds the app, makes the installer and writes `dist\latest.json`.
4. Upload both files to the `releases` bucket in the Supabase dashboard, installer first. Or set `$env:SUPABASE_SERVICE_ROLE_KEY` for that PowerShell window and add `-Publish`; the key is never saved.

Code signing isn't set up yet, so the first run shows Windows SmartScreen's "unknown publisher" notice. A code-signing certificate removes it.

## Profile pictures

Pro users can use a PNG or JPG, or an **animated GIF or MP4** (MP4 on Windows).
- Animations are cropped to a circle and turned into up to 48 frames at 96 px (about 4 seconds, looping at 12.5 fps).
- They're stored as a small JPEG strip in the same Supabase `avatars` slot (under 1 MB), and play in the toolbar badge and the Account panel.
