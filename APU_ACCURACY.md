# APU accuracy: missing and inaccurate features

Audio source audit from 2026-10-30, based on `be0ff4cd64` and updated for
the stereo and sinc-resampler changes. Covers the MCPX APU
(`native/dev/apu/`) and AC'97 controller (`native/dev/aci.c`). Rows marked
"Measured (xemu #3105)" follow raddad772's measurements on a retail console
(ported, see "Fourth batch"); the others are documented or inferred. DSP-program measurements are described below. Line numbers refer
to the audit commit; paths are relative to `native/dev/apu/` unless stated.

The xemu-derived APU has a voice processor (VP) that decodes, resamples,
filters and mixes up to 256 voices into 32 bins. Two DSP56300s handle
DirectSound effects and mixing (GP) and output encoding (EP). By default,
the VP supplies the output (`MON_VP`), with limited DSP interpreter time.
`NC_APU_DSP=1` runs the DSPs to completion and uses their output.

Rows follow implementation order: delivered changes, then work needing more
evidence. Rows 1–4 may explain issue #44 (quiet, off-centre audio in Halo
and Ninja Gaiden Black); none has been isolated in those games.

| # | Part | Xbox | Current state | Effect | Next | Status |
|---|---|---|---|---|---|---|
| 1 | **Default mix: panning** | The GP program mixes the VP's mix bins to the output; DirectSound's 2D sounds go to its speaker bins (0 to 5: front left and right, centre, LFE, back left and right) by their sends. | A 2D voice outside the multipass groups routed to the speakers is mixed by the stereo downmix of its speaker sends: front left and right to their own speaker, the centre to both at -3 dB, each back bin to its own side at -3 dB, each send at `ea x attenuation / 2^headroom` (`vp/stereo_route.h`). Its LFE and effect sends (the 3D crosstalk bins, I3DL2, the effect sends) are left out of that mix, whose effects aren't produced. The route is a sound's, chosen at its first frame with an audible send: the speakers if one goes to a speaker bin, else the monitor's mix, kept as its volumes change (a dry send fading to mute beside an effect send fades the voice out) until its next `VOICE_ON`, and saved with a state (`vp.route`; an older state's voices are chosen again). 3D voices the same way (row 3). Every other voice keeps the monitor's mix, its loudest send's gain on both channels: multipass groups, and voices whose sound began sending only to the LFE or effect bins. | 2D sounds keep their left/right placement, centre and back sends and voices with a reverb send included. Multipass groups are still unpanned by their sends; a voice sending only to the LFE is heard, where a stereo Xbox drops the LFE. | The weights are measured: Halo's DSP image (DirectSound's) downmixes the speaker bins for a stereo console in its EP by exactly these (a 5x2 table at EP Y:0000-0009, no LFE), flat from 100 Hz to 8 kHz (see "Measurements"). | Done (third batch, measured) |
| 2 | **Headroom** | DirectSound gives mix bins a headroom shift (2^n down); what the guest's GP program does with it afterwards is not established for every program. | The default mix divides by each bin's 2^n (`vp/stereo_route.h`), with nothing restoring it. HRTF submixes take their bins' headroom too; `SET_HRTF_HEADROOM` has no measurable effect, and a multipass voice scales its bin back up by its format headroom (measured, xemu #3105). | Sounds on bins with headroom are 6, 12 or 18 dB quieter than without it; whether they are on an Xbox is the open question. | A defined DSP mix reference (an image or measurements mapping bins to final stereo gain) before compensating. Not a universal gain fix: removing it changes levels and clipping. | Partly measured; the GP's use open |
| 3 | **3D audio (HRTF)** | A 31-tap HRTF filter and an interaural delay per 3D voice into the HRTF submixes; the guest's GP program processes those for the speakers. | The filter is implemented (`vp/hrtf.h`). As measured on hardware (xemu #3105): taps are sign-magnitude minifloats (40h is 1/16, 7Fh 31/32), applied as written, not normalized; taps and delay move to a new entry across one frame, read every frame; routes 0-1 carry the left ear and 2-3 the right, routes 4-7 the voice itself; without an entry routes 0-3 are silent and the voice filters as 2D; a 3D voice plays its channel 0. In the default mix the ears go to the speakers by the 3D bins' downmix, as Halo's DSP image does ("Measurements"). A reused voice starts its filter from silence. | A headphone-style image on speakers, placed by its sends and its ears. Folding the ears together is not a fix (it cancels and loses the ear difference); `NC_APU_HRTF=0` is a diagnostic. | Speaker processing past the VP: the guest DSP's. | Measured (xemu #3105) |
| 4 | **Resampling** (pitch) | The VP converts every voice to 48 kHz; its interpolation isn't documented. | By default the hardware's own interpolation (`hardware`): 8-point Lagrange with weights rounded to 1/65536, a 16.16 cursor, and the pitch moving across each frame in its log form, as measured (xemu #3105; `vp/resample.c`). The `hakux_audio_resampler` option keeps sinc (32 Kaiser-windowed taps, `vp/resample_sinc.c`) and linear; they take each frame's pitch as before. | Hardware: what the Xbox does, high frequencies included. Sinc: cleaner than the console. A sound is heard to its last sample with any of them. | - | Measured (xemu #3105) |
| 5 | **GP DSP** (effects, mixing) | A 160 MHz DSP56300 runs DirectSound's image: I3DL2 reverb, crosstalk, the mix. | Execution: xemu's interpreter, 1000 cycles a frame by default (`dsp/gp_ep.c:499`), up to a hardware frame's worth while a CPU thread waits on it (`GP_FRAME_CYCLES`, `dsp/gp_ep.c:462`), to completion with `NC_APU_DSP=1`. Output: its result is used only with `NC_APU_DSP=1` (and no EP). DMA details are unknown (FIXMEs in `dsp/dsp_dma.c`). | No reverb or environment effects in the default output. The full-DSP path is an experimental reference candidate, not a correctness oracle: it has its own interpreter, DMA, routing and timing uncertainties. | Bounded execution and fault handling, guest-program fixtures, and Nova performance data before any default change. | Open |
| 6 | **EP DSP** (outputs) | Encodes Dolby Digital for S/PDIF and feeds the analog output. | Execution: an enabled EP runs 1000 cycles every 8th frame by default, to completion with `NC_APU_DSP=1`; it is not exclusive to that switch. Output: its FIFO 0 is the output only with `NC_APU_DSP=1`. | None in the default output. | With row 5. | Open |
| 7 | **Libretro output** | - | The consumer now removes from its ring only the frames RetroArch's batch callback accepted; the rest are offered again first, in order, and a refusing frontend backs the ring up (the producer waits at its high watermark) instead of losing them (`native/libretro/audio.c`). Before, a call whose frames the frontend took only part of, or none of, dropped the rest (800 frames of a refused period). | No frames lost when the frontend's buffer is full. Not a cause of steady muffling or a channel shift. | - | Done (first batch) |
| 8 | **Per-voice filters** | DLS2 low-pass, parametric EQ, and I3DL2 occlusion / obstruction filtering for 3D voices. | DLS2 without input scaling or waveshaping, unity passband at any resonance; ParaEQ (the SVF as a peaking EQ, Q in `CFG_MISC`); I3DL2's direct and room one-poles, the room path on routes 5 and 7; filter values kept to 1/16 of a 24-bit sample, coefficients as hardware computes them (xemu #3105). | - | - | Measured (xemu #3105) |
| 9 | **Filter-envelope modulation** | The filter envelope modulates pitch and filter cutoff. | Pitch and both filters' cutoffs, the cutoff moving linearly across the frame (xemu #3105). | - | - | Measured (xemu #3105) |
| 10 | **Volume envelope shapes** | Delay, attack, hold, decay, sustain, release in the hardware's curves. | The measured timing and curves: delay and hold 16T + 1 frames, attack to 2048ths in 16R frames, decay by e^(-1/2R) a frame to the sustain level, release by e^(-1/2R) then a straight line to nothing at 15R (xemu #3105). The voice LFOs are implemented too. | - | - | Measured (xemu #3105) |
| 11 | **Volume changes** | Target volumes (`TAR_VOL`); whether the hardware ramps towards them isn't established by the name. | Route gains in 1/16384 on the hardware's attenuation curve (straight lines between whole decibels, nothing from 63 dB), times the envelope's level, ramped across each frame, in the mix bins and the VP's own mix; a new voice fades in from silence (xemu #3105). | - | - | Measured (xemu #3105) |
| 12 | **Phase negation** | Phase-inversion flags for voice send slots 6 and 7 (matrix-surround encoding). These are a voice's send slots, not global mix bins 6 and 7. | Not applied (`vp/vp.c:1913`). | Unverified. | The control bits and their routing semantics, and isolated phase tests, before implementing. | Open (needs evidence) |
| 13 | **AC'97 PCM out** | Programs can play PCM through the AC'97 controller directly (nxdk homebrew, some applications). | Emulated, but its PCM out is consumed and dropped (`native/dev/aci.c:8`); S/PDIF out doesn't advance. | Homebrew that plays through AC'97 is silent. | A synchronized stereo sink, descriptor and interrupt tests, and a defined interaction with the APU's output; nxdk's driver also waits on digital descriptor completion, so advancing analog PCM alone may not complete its queue. | Open |
| 14 | **Voice formats** | PCM 8/16/24/32-bit and Xbox ADPCM, mono and stereo, loops, streams, linked and multipass voices. | Implemented. The ADPCM block size is uncertain, 64 or 65 (`apu_regs.h:336`). | None known. | Validation against hardware. | Implemented, validation outstanding |
| 15 | **Voice notifications** | Status and position reports to DirectSound. | Mostly done; some flags aren't set (`vp/vp.c:134`). | Rare stream stalls. | - | Open |
| 16 | **Console audio mode** | Stereo, mono or surround, and the Dolby Digital / DTS flags, in the EEPROM. | A new EEPROM (`<hdd>/eeprom.bin`) starts at 0: stereo, no Dolby Digital (`native/kernel/ex.c`, offset 0x98). A persisted one is loaded as it is, and titles change it through `ExSaveNonVolatileSetting`. | Right for the Nova's speakers on a new EEPROM. | Leave the user's EEPROM alone; don't advertise multichannel output that isn't produced. | As intended |
| 17 | **Output format** | 48 kHz. | 48 kHz 16-bit stereo to RetroArch, with an optional volume limit. | None. | - | As intended |

## Delivered

**First batch** (no listening tests needed: each follows from explicit
routing, filter-state isolation or the libretro callback contract):

- **HRTF history** (row 3): `hrtf_filter_clear_history` at `VOICE_ON`, and
  for the muted-voice path's resume; coefficients and delays stay.
- **Ordinary 2D stereo panning** (row 1): `vp/stereo_route.h`, called from
  `voice_process`; the monitor's mix is unchanged for every other voice, and
  the full-DSP path doesn't use it.
- **Libretro short writes** (row 7).
- Tests: `tests/hle/test_apu_hrtf.c`, `test_apu_stereo.c` and
  `test_lr_audio.c` (host, also under ASan and UBSan), and
  `tests/native/linux/apu_stereo.c` (`apu-stereo-smoke`, through voice
  processing in the Linux build).

Unchanged by it: the DSP, HRTF, headroom and resampler defaults, voice
positions and notifications, the EEPROM, the frontend frame rate, the worker
count, and the savestate layout (a state load still resets the filters
through `mcpx_apu_vp_reset`).

**Second run** (row 4):

- **Sinc resampler** (`vp/resample_sinc.c`): a 32-tap Kaiser-windowed sinc
  (beta 6) in float, from a table of 128 phases interpolated between, its
  taps through NEON (on x86-64 AVX2 + FMA, bit for bit NEON's, or SSE2:
  `vp/simd.h`); the filter stretched up to 8 times for voices pitched past
  48 kHz. The default; `linear` stays as the `hakux_audio_resampler` core
  option and `NC_APU_RESAMPLER`.
- It meets the spec's criteria, 10 kHz included, where libsamplerate's
  `SRC_SINC_MEDIUM_QUALITY` (tried first) left 10 kHz at -3.25 dB and
  `SRC_SINC_FASTEST` at -22.8 dB. libsamplerate's double-precision filter
  cost 3.3 us a voice-frame on the Nova's X3 and 37 on an A510, 14 to 23
  times linear; this one is 2 to 4 times linear.
- A voice's converter is made at its first `VOICE_ON` (5 KB for sinc, 1 KB
  for linear) and kept for its later sounds: none in a frame.
- **A sound's end** (both qualities): the decoder reaching the end of a
  buffer that doesn't loop, or of a stream that doesn't persist, no longer
  turns the voice off. The voice plays on with silence after the end until
  the resampler's output has passed its last sample and the filter no
  longer reads it; that frame is mixed, then the voice goes off. Before,
  the frame the decoder reached the end in was dropped with what the
  resampler held: short sounds lost most of themselves (a 128-frame sound
  pitched to 96 kHz was silent with sinc), and every one-shot voice its
  last 2 to 3 ms. Ratios are taken across the pitch's whole range with
  its envelope, 1/512 to 512 (clamped to 1/256 .. 256 before).
- Tests: `tests/hle/test_apu_resample.c` (host: response, aliasing, unity,
  chunking, pitch changes, the pitch's whole range and each ratio's rate,
  reset, lookahead, step and tail; `bench` a voice-frame's cost by rate)
  and `tests/native/linux/apu_resample.c` (`apu-resample-smoke`, both
  qualities through voice processing: levels and loops, pitch changes, a
  voice's end heard whole, short sounds heard whole, reuse, a starved
  stream, ADPCM loops; `bench` for throughput and allocations).

**Third batch** (row 1, the stereo downmix):

- **2D voices**: a 2D voice whose sound's first audible frame has an
  audible send to a speaker bin, not only to front left and right, is
  routed to the speakers (`vp_stereo_classify`, the route kept until its
  next `VOICE_ON`, so volume fades don't switch it): the centre to both
  speakers and each back bin to its own side at -3 dB, the LFE and
  non-speaker bins left out (`vp_stereo_gains`). Before, an audible send past front left and
  right, a reverb send among them, gave the voice the monitor's mix: both
  speakers alike, at its loudest send's gain, so a voice with a louder
  reverb send also played too loud. A 2D voice whose every audible send
  goes to the LFE or non-speaker bins keeps that mix, so none goes silent.
  Choosing the route each frame instead (the first version of this batch)
  switched a voice whose dry send faded to mute beside a louder effect
  send to that send's level, centred: a 64 dB step.
- Tests: `tests/hle/test_apu_stereo.c` (the centre and backs at -3 dB, a
  centred sound's power kept, five speaker sends added, LFE, effect,
  crosstalk and multipass sends left out, a voice with only those) and
  `apu-stereo-smoke` (a centre send, a sound panned right by its front
  and back sends, a voice with a louder reverb send, an LFE-only voice; a
  dry send fading out and in beside a reverb send, a sound starting muted,
  a new sound choosing again; the route across a save and load, and an
  older state without it).

**Fourth batch** (rows 2-4 and 8-11): the voice processor as raddad772
measured it on a retail console with APU test programs (xemu PR #3105, its
hardware screenshots and the probes' source attached there), ported to this
core's mixers: HRTF decoding, routing and timing; DLS2, ParaEQ and I3DL2;
the attenuation curve, gain ramps, envelopes and LFOs; the 8-point
Lagrange resampler, now the default; multipass headroom and 24-bit
rounding; output SGEs kept off the voice table; a serial fallback for
multipass layouts DirectSound doesn't use. Each voice's LFOs, ramps and
filter state are saved with a state (`vp.filters`; an older state's voices
resume as at their `VOICE_ON`). Tests: `test_apu_hrtf`, `test_apu_simd`,
`test_apu_resample`, `test_apu_stereo`, `apu-stereo-smoke`,
`apu-resample-smoke` and `libretro-smoke` to the measured behaviour.

## Measurements

**The stereo downmix of DirectSound's DSP image** (Halo, 2026-10-30, the
Nova, `NC_APU_DSP=1`): with Halo's own GP and EP programs running, a 1 kHz,
100 Hz and 8 kHz tone at 0.25 of full scale was put into one mix bin at a
time, every other bin silent, and the outputs recorded. The analog output
(the EP's output FIFO 0, 16-bit stereo):

| Bin | Left | Right |
|---|---|---|
| 0 front left | 0.00 dB | - |
| 1 front right | - | 0.00 dB |
| 2 centre | -3.01 dB | -3.01 dB |
| 3 LFE | - | - |
| 4 back left | -3.01 dB | - |
| 5 back right | - | -3.01 dB |

The same at all three frequencies; "-" is nothing above -80 dB. Through
the 3D bins (at 1 kHz), the GP's crosstalk stage puts each on the speaker
bin of the same place, then the EP downmixes it:

| Bin | Left | Right |
|---|---|---|
| 6 3D front left | -0.03 dB | -24.44 dB (+97 degrees) |
| 7 3D front right | -24.44 dB (+97 degrees) | -0.03 dB |
| 8 3D back left | -3.04 dB | -27.45 dB |
| 9 3D back right | -27.45 dB | -3.04 dB |

The effect sends 11 and 12 went into the LFE bin at -6 dB, so a stereo
console doesn't play them; I3DL2 (10) and the other effect sends gave
nothing in the tone's 64 ms. The EP does
the downmix, by a table in its Y memory (Y:0000-0009: 1 and 0, 0.7071 and
0.7071, 0 and 1, 0.7071 and 0, 0 and 0.7071, in AC-3's channel order L, C,
R, Ls, Rs), read from its memory as well as measured through the core's
DSP interpreter: the two agree. This is the program and its data, not the
hardware's analog stage or level (row 2).

## Next

1. **Listening:** the sinc resampler in the #44 titles on the Nova.
2. **Evidence-driven:** rows 2, 3 (rest), 5, 8-12 and 13, each after its
   semantics are established by a reference, executable vectors or
   recordings. Issue #44 stays open until its cause is isolated in the
   games.
