// Plays songs and preview scenarios on the main wasm module (ctrmml_cmd.js)
// and on the player module (ctrmml_player.wasm), block by block, and requires
// the same output bits, ticks, playing state and error text. Reports the
// player's timings, and with ctrmml_player_heap_probe.wasm, its memory use.
//
//   node tests/player/parity.mjs [options] <wasm-build-dir> [<file.mml or dir>...]
//
// Without paths it plays tests/fixtures. Options:
//   --seconds <n>   longest stretch played from the start (default 60)
//   --verbose       one line per song

import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';

const SAMPLE_RATE = 44100;
const BLOCK = 128;
const MAX_FRAMES = 4096;
const here = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
let seconds = 60;
let verbose = false;
const positional = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--seconds') seconds = Number(args[++i]);
  else if (args[i] === '--verbose') verbose = true;
  else positional.push(args[i]);
}
if (positional.length < 1) {
  console.error('usage: parity.mjs [--seconds n] [--verbose] <wasm-build-dir> [<file.mml or dir>...]');
  process.exit(2);
}
const buildDir = path.resolve(positional[0]);
const songRoots = positional.length > 1
  ? positional.slice(1).map((p) => path.resolve(p))
  : [path.join(here, '..', 'fixtures')];

function findSongs(root) {
  if (fs.statSync(root).isFile()) return [root];
  const out = [];
  for (const entry of fs.readdirSync(root, { withFileTypes: true })) {
    const p = path.join(root, entry.name);
    if (entry.isDirectory()) out.push(...findSongs(p));
    else if (entry.name.endsWith('.mml')) out.push(p);
  }
  return out;
}
const songs = songRoots.flatMap(findSongs).sort();

// ---------------------------------------------------------------------------
// Main module: the Emscripten web build, given its wasm through fetch()
// ---------------------------------------------------------------------------

async function loadMain() {
  const js = fs.readFileSync(path.join(buildDir, 'ctrmml_cmd.js'), 'utf8');
  const wasm = fs.readFileSync(path.join(buildDir, 'ctrmml_cmd.wasm'));
  const fetchWasm = async () =>
    new Response(wasm, { headers: { 'Content-Type': 'application/wasm' } });
  const factory = new Function('fetch', `${js}\nreturn createCtrmmlModule;`)(fetchWasm);
  // The module binds console.error for its compiler warnings when created.
  const consoleError = console.error;
  console.error = () => {};
  const mod = await factory();
  console.error = consoleError;
  const outPtr = mod._malloc(MAX_FRAMES * 8);
  const sizePtr = mod._malloc(4);

  const withString = (str, fn) => {
    const len = mod.lengthBytesUTF8(str) + 1;
    const ptr = mod._malloc(len);
    mod.stringToUTF8(str, ptr, len);
    try {
      return fn(ptr);
    } finally {
      mod._free(ptr);
    }
  };
  const withBytes = (bytes, fn) => {
    const ptr = mod._malloc(bytes.length);
    new Uint8Array(mod.HEAPF32.buffer, ptr, bytes.length).set(bytes);
    try {
      return fn(ptr, bytes.length);
    } finally {
      mod._free(ptr);
    }
  };

  return {
    mod,
    lastError: () => mod.UTF8ToString(mod._ctrmml_cmd_wasm_get_last_error()),
    compile: (text, dir) => withString(text, (t) => withString(dir, (d) => mod._ctrmml_cmd_wasm_compile(t, d))),
    compileAndRelink: (text, dir) =>
      withString(text, (t) => withString(dir, (d) => mod._ctrmml_cmd_wasm_compile_and_relink(t, d))),
    serialize() {
      const ptr = mod._ctrmml_cmd_wasm_serialize_song(sizePtr);
      if (!ptr) return null;
      const size = mod.HEAPU32[sizePtr >> 2];
      return new Uint8Array(mod.HEAPF32.buffer, ptr, size).slice();
    },
    start: (tick) => mod._ctrmml_cmd_wasm_start_playback_at_tick(SAMPLE_RATE, tick),
    stop: () => mod._ctrmml_cmd_wasm_stop_playback(),
    isPlaying: () => mod._ctrmml_cmd_wasm_is_playing(),
    ticks: () => mod._ctrmml_cmd_wasm_get_player_ticks(),
    setChipType: (t) => mod._ctrmml_cmd_wasm_set_ym2612_chip_type(t),
    setMuteMask: (chip, mask) => mod._ctrmml_cmd_wasm_set_mute_mask(chip, mask),
    setStopAtPcmEnd: (on) => mod._ctrmml_cmd_wasm_set_stop_at_pcm_end(on),
    // Interleaved output bits; empty when nothing played.
    render(frames) {
      const n = mod._ctrmml_cmd_wasm_render_audio(outPtr, frames);
      return new Uint32Array(mod.HEAPF32.buffer, outPtr, n * 2);
    },
    previewInit: (rate) => mod._preview_init(rate),
    previewDeinit: () => mod._preview_deinit(),
    previewLoadFm: (bytes) => withBytes(bytes, (p, n) => mod._preview_load_fm(p, n)),
    previewLoadPsg: (bytes) => withBytes(bytes, (p, n) => mod._preview_load_psg(p, n)),
    previewSetMode: (m) => mod._preview_set_mode(m),
    previewNoteOn: (n, v) => mod._preview_note_on(n, v),
    previewNoteOnAttenuation: (n, a) => mod._preview_note_on_attenuation(n, a),
    previewNoteOff: (n) => mod._preview_note_off(n),
    previewAllNotesOff: () => mod._preview_all_notes_off(),
    previewSetFmOpMask: (m) => mod._preview_set_fm_op_mask(m),
    previewSetMono: (m) => mod._preview_set_mono(m),
  };
}

// ---------------------------------------------------------------------------
// Player module: instantiated the way an AudioWorkletProcessor would
// ---------------------------------------------------------------------------

const ERRNO_BADF = 8;
const ERRNO_NOSYS = 52;

function wasiImports(memory, log) {
  return {
    clock_time_get(_id, _precision, out) {
      new DataView(memory().buffer).setBigUint64(out, 0n, true);
      return 0;
    },
    environ_sizes_get(count, size) {
      const view = new DataView(memory().buffer);
      view.setUint32(count, 0, true);
      view.setUint32(size, 0, true);
      return 0;
    },
    environ_get: () => 0,
    fd_write(_fd, iovs, count, written) {
      const view = new DataView(memory().buffer);
      let total = 0;
      for (let i = 0; i < count; i++) {
        const ptr = view.getUint32(iovs + i * 8, true);
        const len = view.getUint32(iovs + i * 8 + 4, true);
        log.push(Buffer.from(memory().buffer, ptr, len).toString());
        total += len;
      }
      view.setUint32(written, total, true);
      return 0;
    },
    fd_read: () => ERRNO_BADF,
    fd_seek: () => ERRNO_BADF,
    fd_close: () => ERRNO_BADF,
  };
}

function instantiatePlayer(module) {
  let instance = null;
  const log = [];
  const wasi = wasiImports(() => instance.exports.memory, log);
  const imports = {};
  for (const entry of WebAssembly.Module.imports(module)) {
    imports[entry.module] ??= {};
    imports[entry.module][entry.name] = wasi[entry.name] ?? (() => ERRNO_NOSYS);
  }
  instance = new WebAssembly.Instance(module, imports);
  const ex = instance.exports;
  ex._initialize?.();
  if (ex.player_init(SAMPLE_RATE) !== 1) throw new Error('player_init failed');

  const toInbox = (bytes) => {
    const ptr = ex.player_inbox(bytes.length);
    if (!ptr) throw new Error(`no room for ${bytes.length} bytes`);
    new Uint8Array(ex.memory.buffer, ptr, bytes.length).set(bytes);
    return ptr;
  };

  return {
    ex,
    log,
    lastError: () =>
      Buffer.from(ex.memory.buffer, ex.player_last_error(), ex.player_last_error_size()).toString(),
    load(bytes, tick) {
      toInbox(bytes);
      return ex.player_load(bytes.length, tick);
    },
    relink(bytes) {
      toInbox(bytes);
      return ex.player_relink(bytes.length);
    },
    stop: () => ex.player_stop(),
    isPlaying: () => ex.player_is_playing(),
    ticks: () => ex.player_ticks(),
    setChipType: (t) => ex.player_set_ym2612_chip_type(t),
    setMuteMask: (chip, mask) => ex.player_set_mute_mask(chip, mask),
    setStopAtPcmEnd: (on) => ex.player_set_stop_at_pcm_end(on),
    // Planar output bits: left, then right.
    render(frames) {
      const ptr = ex.player_render(frames);
      return [
        new Uint32Array(ex.memory.buffer, ptr, frames),
        new Uint32Array(ex.memory.buffer, ptr + frames * 4, frames),
      ];
    },
    previewInit: (rate) => ex.preview_init(rate),
    previewDeinit: () => ex.preview_deinit(),
    previewLoadFm: (bytes) => ex.preview_load_fm(toInbox(bytes), bytes.length),
    previewLoadPsg: (bytes) => ex.preview_load_psg(toInbox(bytes), bytes.length),
    previewSetMode: (m) => ex.preview_set_mode(m),
    previewNoteOn: (n, v) => ex.preview_note_on(n, v),
    previewNoteOnAttenuation: (n, a) => ex.preview_note_on_attenuation(n, a),
    previewNoteOff: (n) => ex.preview_note_off(n),
    previewAllNotesOff: () => ex.preview_all_notes_off(),
    previewSetFmOpMask: (m) => ex.preview_set_fm_op_mask(m),
    previewSetMono: (m) => ex.preview_set_mono(m),
  };
}

// ---------------------------------------------------------------------------
// Lockstep comparison
// ---------------------------------------------------------------------------

class Mismatch extends Error {}

/** Renders `frames` on both and throws on the first difference. */
function renderBoth(main, player, frames, where) {
  const m = main.render(frames);
  const [l, r] = player.render(frames);
  const mainFrames = m.length / 2;
  for (let i = 0; i < frames; i++) {
    const ml = i < mainFrames ? m[i * 2] : 0;
    const mr = i < mainFrames ? m[i * 2 + 1] : 0;
    if (ml !== l[i] || mr !== r[i]) {
      throw new Mismatch(`${where}: sample ${i} differs (main ${ml.toString(16)}/${mr.toString(16)}, player ${l[i].toString(16)}/${r[i].toString(16)})`);
    }
  }
  if (main.isPlaying() !== player.isPlaying()) {
    throw new Mismatch(`${where}: playing ${main.isPlaying()} vs ${player.isPlaying()}`);
  }
  if (main.ticks() !== player.ticks()) {
    throw new Mismatch(`${where}: ticks ${main.ticks()} vs ${player.ticks()}`);
  }
  return mainFrames;
}

/** Plays up to `blocks` blocks, or until the song ends. Returns blocks played. */
function playBoth(main, player, blocks, where) {
  let played = 0;
  while (played < blocks && main.isPlaying()) {
    renderBoth(main, player, BLOCK, `${where} block ${played}`);
    played++;
  }
  // Once the song has ended both must stay silent.
  renderBoth(main, player, BLOCK, `${where} after end`);
  if (main.lastError() !== player.lastError()) {
    throw new Mismatch(`${where}: error "${main.lastError()}" vs "${player.lastError()}"`);
  }
  return played;
}

/** Times only the player, rendering `blocks` blocks or until the song ends. */
function timePlayer(player, blocks) {
  const times = [];
  for (let i = 0; i < blocks && player.isPlaying(); i++) {
    const t0 = performance.now();
    player.ex.player_render(BLOCK);
    times.push(performance.now() - t0);
  }
  return times;
}

// ---------------------------------------------------------------------------
// Songs
// ---------------------------------------------------------------------------

const PCM_PATTERN = /@\d+\s+pcm\s+("([^"]+)"|(\S+))/gi;

/** Copies a song and the samples it names into MEMFS, at their real paths. */
function mirrorSong(mod, file) {
  const dir = path.dirname(file);
  const text = fs.readFileSync(file, 'utf8');
  const write = (p, data) => {
    mod.FS.mkdirTree(path.dirname(p));
    mod.FS.writeFile(p, data);
  };
  write(file, text);
  for (const match of text.matchAll(PCM_PATTERN)) {
    const sample = path.resolve(dir, match[2] ?? match[3]);
    if (fs.existsSync(sample) && fs.statSync(sample).isFile()) write(sample, fs.readFileSync(sample));
  }
  return { text, dir };
}

function blocksFor(secs) {
  return Math.ceil((secs * SAMPLE_RATE) / BLOCK);
}

function summarize(times) {
  if (!times.length) return { mean: 0, max: 0 };
  let sum = 0;
  let max = 0;
  for (const t of times) {
    sum += t;
    if (t > max) max = t;
  }
  return { mean: sum / times.length, max };
}

/** Loads, plays and relinks a song on a heap probe player, with the preview on. */
function exerciseProbe(probe, blob, tick) {
  probe.previewInit(SAMPLE_RATE);
  probe.load(blob, 0);
  timePlayer(probe, blocksFor(5));
  probe.relink(blob);
  timePlayer(probe, blocksFor(1));
  probe.load(blob, tick);
  timePlayer(probe, blocksFor(1));
}

async function runSongs(main, playerModule, probeModule) {
  const results = { passed: 0, failed: [], skipped: [], errors: [], startErrors: [] };
  const stats = [];
  // One probe for every song in turn, as a page would use the player.
  const longProbe = probeModule ? instantiatePlayer(probeModule) : null;
  let previous = null;
  for (const file of songs) {
    const name = path.relative(process.cwd(), file);
    const { text, dir } = mirrorSong(main.mod, file);
    if (main.compile(text, dir) !== 0) {
      results.skipped.push(`${name}: ${main.lastError().split('\n')[0]}`);
      continue;
    }
    const z0 = performance.now();
    const blob = main.serialize();
    const serializeMs = performance.now() - z0;
    const player = instantiatePlayer(playerModule);
    try {
      // From the start. A song that cannot start must fail the same way.
      const mainRc = main.start(0);
      const t0 = performance.now();
      const playerRc = player.load(blob, 0);
      const loadMs = performance.now() - t0;
      if (mainRc !== playerRc || main.lastError() !== player.lastError()) {
        throw new Mismatch(`start ${mainRc} "${main.lastError()}", load ${playerRc} "${player.lastError()}"`);
      }
      if (mainRc !== 0) {
        results.startErrors.push(`${name}: ${main.lastError()}`);
        results.passed++;
        previous = { text, dir };
        continue;
      }
      const played = playBoth(main, player, blocksFor(seconds), 'start');
      if (main.lastError()) results.errors.push(`${name}: ${main.lastError()}`);

      // From a tick a third of the way in.
      main.start(0);
      player.load(blob, 0);
      for (let i = 0; i < Math.floor(played / 3); i++) renderBoth(main, player, BLOCK, 'seek');
      const midTick = main.ticks();
      main.stop();
      player.stop();
      if (main.start(midTick) !== 0) throw new Mismatch(`start at ${midTick}: ${main.lastError()}`);
      if (player.load(blob, midTick) !== 0) throw new Mismatch(`load at ${midTick}: ${player.lastError()}`);
      playBoth(main, player, blocksFor(Math.min(seconds, 20)), `tick ${midTick}`);

      // Relinked a third of the way in: to the same song, then to the
      // previous song.
      let relinkMs = 0;
      const relinkTo = [{ text, dir, label: 'itself' }];
      if (previous) relinkTo.push({ ...previous, label: 'previous song' });
      for (const target of relinkTo) {
        main.compile(text, dir);
        main.start(0);
        player.load(blob, 0);
        for (let i = 0; i < Math.floor(played / 3) && main.isPlaying(); i++) {
          renderBoth(main, player, BLOCK, `before relink to ${target.label}`);
        }
        const mainRc = main.compileAndRelink(target.text, target.dir);
        const targetBlob = main.serialize();
        const r0 = performance.now();
        const playerRc = player.relink(targetBlob);
        relinkMs = Math.max(relinkMs, performance.now() - r0);
        if (mainRc !== playerRc) throw new Mismatch(`relink to ${target.label}: ${mainRc} vs ${playerRc}`);
        if (main.lastError() !== player.lastError()) {
          throw new Mismatch(`relink to ${target.label}: "${main.lastError()}" vs "${player.lastError()}"`);
        }
        playBoth(main, player, blocksFor(Math.min(seconds, 20)), `relinked to ${target.label}`);
      }
      main.stop();

      // Player timing on its own, from a fresh instance.
      globalThis.gc?.();
      const solo = instantiatePlayer(playerModule);
      const s0 = performance.now();
      solo.load(blob, 0);
      const soloLoadMs = performance.now() - s0;
      const soloTimes = timePlayer(solo, blocksFor(seconds));

      let heapPeak = null;
      if (probeModule) {
        const probe = instantiatePlayer(probeModule);
        exerciseProbe(probe, blob, midTick);
        heapPeak = probe.ex.probe_heap_base() + probe.ex.probe_heap_peak();
        exerciseProbe(longProbe, blob, midTick);
      }

      stats.push({
        name,
        printed: player.log.length + solo.log.length,
        blobBytes: blob.length,
        serializeMs,
        loadMs: Math.min(loadMs, soloLoadMs),
        relinkMs,
        firstBlockMs: soloTimes[0] ?? 0,
        ...summarize(soloTimes.slice(1)),
        p99: soloTimes.length > 1 ? [...soloTimes.slice(1)].sort((a, b) => a - b)[Math.floor((soloTimes.length - 2) * 0.99)] : 0,
        heapPeak,
      });
      results.passed++;
      if (verbose) console.log(`ok   ${name} (${played} blocks, blob ${blob.length} B)`);
    } catch (e) {
      if (!(e instanceof Mismatch)) throw e;
      results.failed.push(`${name}: ${e.message}`);
      console.log(`FAIL ${name}: ${e.message}`);
      main.stop();
    }
    previous = { text, dir };
  }
  const longPeak = longProbe ? longProbe.ex.probe_heap_base() + longProbe.ex.probe_heap_peak() : null;
  return { results, stats, longPeak };
}

// ---------------------------------------------------------------------------
// Preview scenarios (as in ctrmml-golden's preview_hash.cpp)
// ---------------------------------------------------------------------------

const PATCH_A = [
  0x71, 0x0d, 0x33, 0x01, 0x5f, 0x99, 0x5f, 0x94, 0x05, 0x05, 0x05, 0x07,
  0x02, 0x02, 0x02, 0x02, 0x11, 0x11, 0x11, 0xa6, 0x00, 0x00, 0x00, 0x00,
  0x23, 0x2d, 0x26, 0x00, 0x32, 0x30];
const PATCH_B = [
  0x01, 0x02, 0x04, 0x01, 0x1f, 0x1f, 0x1f, 0x1f, 0x00, 0x08, 0x00, 0x04,
  0x00, 0x03, 0x00, 0x00, 0x0f, 0x2f, 0x0f, 0x3f, 0x00, 0x0a, 0x00, 0x00,
  0x10, 0x08, 0x14, 0x04, 0x3f, 0x32];
const PATCH_C = [
  0x31, 0x32, 0x71, 0x72, 0x1c, 0x1c, 0x1f, 0x1f, 0x0a, 0x0a, 0x05, 0x05,
  0x01, 0x01, 0x01, 0x01, 0x27, 0x27, 0x17, 0x17, 0x00, 0x00, 0x00, 0x00,
  0x18, 0x7f, 0x05, 0x08, 0x24, 0x34];
const ENV_SUSTAIN_RELEASE = [0x2f, 0x23, 0x15, 0x01, 0x16, 0x27, 0x38, 0x02, 0x04, 0x00];
const ENV_LOOP_HOLD = [0x10, 0x31, 0x22, 0x13, 0x24, 0x02, 0x01, 0x00];
const ENV_SUSTAIN_LOOP = [0x30, 0x22, 0x01, 0x14, 0x25, 0x02, 0x02, 0x00];
const ENV_DECAY = [0x3f, 0x2a, 0x25, 0x14, 0x12, 0x11, 0x00];

/** Applies each call to both modules; render() compares the output. */
function lockstep(main, player, scenario) {
  let blocks = 0;
  const both = new Proxy({}, {
    get(_, name) {
      if (name === 'render') {
        return (count, block = BLOCK) => {
          while (count > 0) {
            const n = Math.min(block, count);
            renderBoth(main, player, n, `${scenario} block ${blocks++}`);
            count -= n;
          }
        };
      }
      return (...a) => {
        main[name](...a);
        player[name](...a);
      };
    },
  });
  return both;
}

const previewScenarios = {
  fm_poly(s, chip, patch) {
    s.setChipType(chip);
    s.previewInit(44100);
    s.previewSetMode(0);
    s.previewLoadFm(patch);
    s.previewNoteOn(60, 100);
    s.render(4410);
    s.previewNoteOn(64, 90);
    s.previewNoteOn(67, 127);
    s.render(8820);
    s.previewNoteOff(64);
    s.render(4410);
    for (let n = 70; n < 77; n++) s.previewNoteOn(n, 40 + n);
    s.render(4410);
    s.previewNoteOnAttenuation(48, 20);
    s.previewNoteOnAttenuation(36, 200);
    s.render(4410);
    s.previewNoteOn(60, 100);
    s.render(2205, 37);
    for (let n = 0; n < 128; n++) s.previewNoteOff(n);
    s.render(30000);
    s.previewNoteOn(72, 64);
    s.render(2000);
    s.previewAllNotesOff();
    s.render(4410);
  },
  fm_chip_switch(s) {
    s.setChipType(0);
    s.previewInit(44100);
    s.previewLoadFm(PATCH_A);
    s.previewNoteOn(57, 110);
    s.previewNoteOn(61, 110);
    s.render(6000);
    s.setChipType(1);
    s.render(6000);
    s.setChipType(0);
    s.previewLoadFm(PATCH_C);
    s.previewNoteOn(64, 110);
    s.render(6000);
    s.previewAllNotesOff();
    s.render(1000);
  },
  fm_mono(s, chip) {
    s.setChipType(chip);
    s.previewInit(44100);
    s.previewSetMono(1);
    s.previewLoadFm(PATCH_B);
    s.previewNoteOn(60, 100);
    s.render(1000, 64);
    s.previewNoteOn(62, 100);
    s.render(3, 1);
    s.render(2000, 64);
    s.previewNoteOff(60);
    s.previewNoteOff(62);
    s.render(1000, 64);
    s.previewNoteOn(65, 100);
    s.previewNoteOff(65);
    s.render(1000, 64);
    s.previewNoteOn(67, 80);
    s.render(10, 1);
    s.previewNoteOn(67, 80);
    s.render(1500, 128);
    s.previewNoteOnAttenuation(69, 30);
    s.render(1, 1);
    s.render(1500, 128);
    s.previewSetMono(0);
    s.previewNoteOn(60, 100);
    s.previewNoteOn(64, 100);
    s.render(2000);
    s.previewSetMono(1);
    s.previewNoteOn(55, 100);
    s.render(30000);
    s.previewAllNotesOff();
    s.render(30000);
  },
  fm_op_mask(s) {
    s.setChipType(0);
    s.previewInit(44100);
    s.previewLoadFm(PATCH_C);
    s.previewSetFmOpMask(0x05);
    s.previewNoteOn(60, 100);
    s.render(4000);
    s.previewSetFmOpMask(0x0a);
    s.previewNoteOn(67, 100);
    s.render(4000);
    s.previewSetFmOpMask(0xff);
    s.previewNoteOn(72, 100);
    s.render(4000);
    s.previewSetMono(1);
    s.previewSetFmOpMask(0x08);
    s.previewNoteOn(48, 100);
    s.render(4000, 100);
  },
  psg_tone(s, env, mono, rate) {
    s.setChipType(0);
    s.previewInit(rate);
    s.previewLoadPsg(env);
    s.previewSetMono(mono ? 1 : 0);
    s.previewSetMode(1);
    s.previewNoteOn(60, 100);
    s.render(2000);
    s.previewNoteOn(64, 100);
    s.render(2000);
    s.previewNoteOn(67, 100);
    s.previewNoteOn(72, 100);
    s.render(44100);
    s.previewNoteOff(64);
    s.render(22050);
    s.previewNoteOff(67);
    s.previewNoteOff(72);
    s.previewNoteOff(60);
    s.render(66150);
    s.previewNoteOn(48, 100);
    s.previewNoteOn(84, 100);
    s.render(10000, 61);
    s.previewNoteOff(84);
    s.render(1000);
    s.previewNoteOn(84, 100);
    s.render(5000);
    s.previewAllNotesOff();
    s.render(30000);
    s.previewNoteOn(30, 100);
    s.previewNoteOff(30);
    s.render(40000);
  },
  psg_noise(s, mode, env) {
    s.setChipType(0);
    s.previewInit(44100);
    s.previewLoadPsg(env);
    s.previewSetMode(mode);
    s.previewNoteOn(60, 100);
    s.render(22050);
    s.previewNoteOff(60);
    s.render(44100);
    s.previewNoteOn(61, 100);
    s.render(5000);
    s.previewNoteOn(66, 100);
    s.render(20000);
    s.previewNoteOff(61);
    s.render(5000);
    s.previewNoteOff(66);
    s.render(30000);
    s.previewNoteOn(45, 100);
    s.render(3000);
    s.previewAllNotesOff();
    s.render(25000);
  },
  mixed_modes(s) {
    s.setChipType(0);
    s.previewInit(48000);
    s.previewLoadFm(PATCH_A);
    s.previewLoadPsg(ENV_SUSTAIN_RELEASE);
    s.previewNoteOn(60, 100);
    s.render(4000);
    s.previewSetMode(1);
    s.previewNoteOn(62, 100);
    s.render(4000);
    s.previewSetMode(3);
    s.previewNoteOn(70, 100);
    s.render(4000);
    s.previewSetMode(0);
    s.previewNoteOn(64, 100);
    s.render(4000);
    s.previewSetMode(4);
    s.previewNoteOn(50, 100);
    s.render(4000);
    s.previewDeinit();
    s.render(100);
    s.previewNoteOn(60, 100);
    s.previewInit(22050);
    s.previewNoteOn(60, 100);
    s.render(2000);
    s.previewLoadFm(PATCH_B);
    s.previewSetMode(0);
    s.previewNoteOn(60, 100);
    s.render(4000);
  },
};

function runPreview(main, playerModule) {
  const failed = [];
  const runs = [
    ['fm_poly_ym2612_A', (s) => previewScenarios.fm_poly(s, 0, PATCH_A)],
    ['fm_poly_ym3438_A', (s) => previewScenarios.fm_poly(s, 1, PATCH_A)],
    ['fm_poly_ym2612_B', (s) => previewScenarios.fm_poly(s, 0, PATCH_B)],
    ['fm_poly_ym3438_C', (s) => previewScenarios.fm_poly(s, 1, PATCH_C)],
    ['fm_chip_switch', (s) => previewScenarios.fm_chip_switch(s)],
    ['fm_mono_ym2612', (s) => previewScenarios.fm_mono(s, 0)],
    ['fm_mono_ym3438', (s) => previewScenarios.fm_mono(s, 1)],
    ['fm_op_mask', (s) => previewScenarios.fm_op_mask(s)],
    ['psg_sustain_release', (s) => previewScenarios.psg_tone(s, ENV_SUSTAIN_RELEASE, false, 44100)],
    ['psg_sustain_release_mono', (s) => previewScenarios.psg_tone(s, ENV_SUSTAIN_RELEASE, true, 44100)],
    ['psg_loop_hold', (s) => previewScenarios.psg_tone(s, ENV_LOOP_HOLD, false, 44100)],
    ['psg_sustain_loop', (s) => previewScenarios.psg_tone(s, ENV_SUSTAIN_LOOP, false, 48000)],
    ['psg_decay_mono', (s) => previewScenarios.psg_tone(s, ENV_DECAY, true, 44100)],
    ['noise2_sustain_release', (s) => previewScenarios.psg_noise(s, 2, ENV_SUSTAIN_RELEASE)],
    ['noise3_loop_hold', (s) => previewScenarios.psg_noise(s, 3, ENV_LOOP_HOLD)],
    ['noise4_sustain_loop', (s) => previewScenarios.psg_noise(s, 4, ENV_SUSTAIN_LOOP)],
    ['noise2_decay', (s) => previewScenarios.psg_noise(s, 2, ENV_DECAY)],
    ['mixed_modes', (s) => previewScenarios.mixed_modes(s)],
  ];
  const player = instantiatePlayer(playerModule);
  for (const [name, run] of runs) {
    try {
      run(lockstep(main, player, name));
    } catch (e) {
      if (!(e instanceof Mismatch)) throw e;
      failed.push(`${name}: ${e.message}`);
    }
    main.previewDeinit();
    player.previewDeinit();
  }
  main.setChipType(0);
  player.setChipType(0);
  return { count: runs.length, failed };
}

/** Preview notes over a playing song, with mute masks and a chip switch. */
function runSongWithPreview(main, playerModule, file) {
  const { text, dir } = mirrorSong(main.mod, file);
  if (main.compile(text, dir) !== 0) return `compile failed: ${main.lastError()}`;
  const blob = main.serialize();
  const player = instantiatePlayer(playerModule);
  const s = lockstep(main, player, 'song with preview');
  try {
    s.previewInit(SAMPLE_RATE);
    s.previewLoadFm(PATCH_A);
    main.start(0);
    player.load(blob, 0);
    s.render(BLOCK * 50);
    s.previewNoteOn(60, 100);
    s.render(BLOCK * 100);
    s.setMuteMask(2, 0x3c);
    s.setChipType(1);
    s.previewNoteOn(67, 90);
    s.render(BLOCK * 100);
    s.setMuteMask(2, 0);
    s.setMuteMask(0, 0x0f);
    s.setChipType(0);
    s.previewNoteOff(60);
    s.previewNoteOff(67);
    s.render(BLOCK * 400);
    main.stop();
    player.stop();
    s.render(BLOCK * 100);
    s.previewDeinit();
    return null;
  } catch (e) {
    if (!(e instanceof Mismatch)) throw e;
    return e.message;
  } finally {
    main.stop();
    main.previewDeinit();
  }
}

// ---------------------------------------------------------------------------

const main = await loadMain();
const playerModule = new WebAssembly.Module(fs.readFileSync(path.join(buildDir, 'ctrmml_player.wasm')));
const probePath = path.join(buildDir, 'ctrmml_player_heap_probe.wasm');
const probeModule = fs.existsSync(probePath) ? new WebAssembly.Module(fs.readFileSync(probePath)) : null;

const imports = WebAssembly.Module.imports(playerModule).map((e) => `${e.module}.${e.name}`);
console.log(`player: ${fs.statSync(path.join(buildDir, 'ctrmml_player.wasm')).size} bytes, ` +
  `memory ${instantiatePlayer(playerModule).ex.memory.buffer.byteLength} bytes`);
console.log(`imports: ${imports.join(', ')}`);

const preview = runPreview(main, playerModule);
console.log(`preview: ${preview.count - preview.failed.length}/${preview.count} scenarios identical`);
for (const f of preview.failed) console.log(`  FAIL ${f}`);

const { results, stats, longPeak } = await runSongs(main, playerModule, probeModule);
console.log(`songs: ${results.passed}/${results.passed + results.failed.length} identical ` +
  `(${results.skipped.length} did not compile)`);
for (const s of results.skipped) console.log(`  skipped ${s}`);
for (const e of results.startErrors) console.log(`  start error, same on both: ${e}`);
for (const e of results.errors) console.log(`  playback error, same on both: ${e}`);

const mixed = songs.find((f) => f.endsWith('song_blob_fm_psg.mml')) ?? songs[0];
const mixedError = runSongWithPreview(main, playerModule, mixed);
console.log(`song with preview (${path.basename(mixed)}): ${mixedError ? `FAIL ${mixedError}` : 'identical'}`);

if (stats.length) {
  const heaviest = [...stats].sort((a, b) => b.blobBytes - a.blobBytes).slice(0, 5);
  const slowest = [...stats].sort((a, b) => b.mean - a.mean).slice(0, 5);
  const slowestLoads = [...stats].sort((a, b) => b.loadMs - a.loadMs).slice(0, 5);
  const fmt = (s) => `  ${s.name}: blob ${s.blobBytes} B, serialize ${s.serializeMs.toFixed(2)} ms, ` +
    `load ${s.loadMs.toFixed(2)} ms, relink ${s.relinkMs.toFixed(2)} ms, ` +
    `first block ${s.firstBlockMs.toFixed(3)} ms, block mean ${s.mean.toFixed(4)} ms ` +
    `p99 ${s.p99.toFixed(3)} ms max ${s.max.toFixed(3)} ms` +
    (s.heapPeak ? `, memory peak ${(s.heapPeak / 1048576).toFixed(2)} MiB` : '');
  console.log('largest songs:');
  heaviest.forEach((s) => console.log(fmt(s)));
  console.log('slowest songs per block:');
  slowest.forEach((s) => console.log(fmt(s)));
  console.log('slowest loads:');
  slowestLoads.forEach((s) => console.log(fmt(s)));
  const printed = stats.filter((s) => s.printed);
  console.log(`songs whose playback printed to stdout: ${printed.length}` +
    (printed.length ? ` (${printed.map((s) => s.name).join(', ')})` : ''));
  const all = summarize(stats.map((s) => s.mean));
  console.log(`block mean over songs: ${all.mean.toFixed(4)} ms; ` +
    `worst block ${Math.max(...stats.map((s) => s.max)).toFixed(3)} ms; ` +
    `worst p99 ${Math.max(...stats.map((s) => s.p99)).toFixed(3)} ms; ` +
    `worst load ${Math.max(...stats.map((s) => s.loadMs)).toFixed(2)} ms; ` +
    `worst relink ${Math.max(...stats.map((s) => s.relinkMs)).toFixed(2)} ms`);
  if (probeModule) {
    const peak = Math.max(...stats.map((s) => s.heapPeak));
    console.log(`memory peak: ${(peak / 1048576).toFixed(2)} MiB for one song, ` +
      `${(longPeak / 1048576).toFixed(2)} MiB for every song in turn on one player`);
  }
}

const ok = !preview.failed.length && !results.failed.length && !mixedError;
process.exit(ok ? 0 : 1);
