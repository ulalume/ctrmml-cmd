// Plays songs and preview scenarios on the player module (ctrmml_player.wasm)
// and on its native reference (player-reference: the same Playback class, with
// the songs compiled natively), and requires the same output bits, ticks,
// playing state and error text. The player gets each song as the main wasm
// module (ctrmml_cmd.js) packs it. Reports the player's timings, and with
// ctrmml_player_heap_probe.wasm, its memory use.
//
//   node tests/player/parity.mjs [options] <wasm-build-dir> [<file.mml or dir>...]
//
// Without paths it plays tests/fixtures. Options:
//   --reference <path>  the player-reference executable of a native build
//   --seconds <n>       longest stretch played from the start (default 60)
//   --verbose           one line per song

import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { performance } from 'node:perf_hooks';

const SAMPLE_RATE = 44100;
const BLOCK = 128;
const here = path.dirname(fileURLToPath(import.meta.url));

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

const args = process.argv.slice(2);
let seconds = 60;
let verbose = false;
let reference = null;
const positional = [];
for (let i = 0; i < args.length; i++) {
  if (args[i] === '--seconds') seconds = Number(args[++i]);
  else if (args[i] === '--reference') reference = path.resolve(args[++i]);
  else if (args[i] === '--verbose') verbose = true;
  else positional.push(args[i]);
}
if (positional.length < 1 || !reference) {
  console.error('usage: parity.mjs --reference <player-reference> [--seconds n] [--verbose] <wasm-build-dir> [<file.mml or dir>...]');
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
// Main module: compiles and packs the songs, given its wasm through fetch()
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

  return {
    mod,
    lastError: () => mod.UTF8ToString(mod._ctrmml_cmd_wasm_get_last_error()),
    compile: (text, dir) => withString(text, (t) => withString(dir, (d) => mod._ctrmml_cmd_wasm_compile(t, d))),
    pack() {
      const ptr = mod._ctrmml_cmd_wasm_pack_song(sizePtr);
      if (!ptr) return null;
      const bytes = mod.HEAPU8.slice(ptr, ptr + mod.HEAPU32[sizePtr >> 2]);
      mod._free(ptr);
      return bytes;
    },
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
// Sessions: calls on the player, recorded with what they returned, for the
// native reference to replay. See tests/player/reference.cpp.
// ---------------------------------------------------------------------------

const script = [];
const expected = [];

/** FNV-1a over 32-bit words, as reference.cpp computes it. */
function hashWords(hash, words) {
  for (let i = 0; i < words.length; i++) hash = Math.imul(hash ^ words[i], 16777619);
  return hash;
}

class Session {
  constructor(playerModule, name) {
    this.player = instantiatePlayer(playerModule);
    this.name = name;
    this.blob = null;
    this.call(`reset ${SAMPLE_RATE}`);
  }

  call(line) {
    script.push(line);
  }

  expect(line, where) {
    expected.push({ line, where: `${this.name}: ${where}` });
  }

  state(hash) {
    const p = this.player;
    return `${(hash >>> 0).toString(16).padStart(8, '0')} ${p.isPlaying()} ${p.ticks() >>> 0} ${p.lastError()}`;
  }

  renderBlock(frames, hash) {
    const [l, r] = this.player.render(frames);
    hash = hashWords(hash, l);
    hash = hashWords(hash, r);
    return hashWords(hash, [this.player.isPlaying(), this.player.ticks()]);
  }

  /** The song for load() and relink(): `file` as the main module packed it. */
  song(file, blob) {
    this.blob = blob;
    this.call(`compile ${file}`);
    this.expect('compile 0', `compile ${path.basename(file)}`);
  }

  load(tick, where) {
    const rc = this.player.load(this.blob, tick);
    this.call(`load ${tick}`);
    this.expect(`load ${rc} ${this.player.lastError()}`, where);
    return rc;
  }

  relink(where) {
    const rc = this.player.relink(this.blob);
    this.call('relink');
    this.expect(`relink ${rc} ${this.player.lastError()}`, where);
    return rc;
  }

  stop() {
    this.player.stop();
    this.call('stop');
  }

  /** Renders blocks while the song plays, at most `maxBlocks`. Returns how many. */
  play(maxBlocks, where) {
    let hash = 2166136261;
    let blocks = 0;
    while (blocks < maxBlocks && this.player.isPlaying()) {
      hash = this.renderBlock(BLOCK, hash);
      blocks++;
    }
    this.call(`play ${maxBlocks}`);
    this.expect(`play ${blocks} ${this.state(hash)}`, where);
    return blocks;
  }

  /** Renders `count` frames in calls of `block` frames. */
  render(count, block = BLOCK, where = 'render') {
    const runs = [[block, Math.floor(count / block)]];
    if (count % block) runs.push([count % block, 1]);
    for (const [frames, times] of runs) {
      let hash = 2166136261;
      for (let i = 0; i < times; i++) hash = this.renderBlock(frames, hash);
      this.call(`render ${frames} ${times}`);
      this.expect(`render ${this.state(hash)}`, `${where} (${times} x ${frames} frames)`);
    }
  }

  setChipType(t) { this.player.setChipType(t); this.call(`chip ${t}`); }
  setMuteMask(chip, mask) { this.player.setMuteMask(chip, mask); this.call(`mute ${chip} ${mask >>> 0}`); }
  setStopAtPcmEnd(on) { this.player.setStopAtPcmEnd(on); this.call(`pcm_end ${on}`); }
  previewInit(rate) { this.player.previewInit(rate); this.call(`preview_init ${rate}`); }
  previewDeinit() { this.player.previewDeinit(); this.call('preview_deinit'); }
  previewLoadFm(bytes) { this.player.previewLoadFm(bytes); this.call(`preview_fm ${hex(bytes)}`); }
  previewLoadPsg(bytes) { this.player.previewLoadPsg(bytes); this.call(`preview_psg ${hex(bytes)}`); }
  previewSetMode(m) { this.player.previewSetMode(m); this.call(`preview_mode ${m}`); }
  previewNoteOn(n, v) { this.player.previewNoteOn(n, v); this.call(`note_on ${n} ${v}`); }
  previewNoteOnAttenuation(n, a) { this.player.previewNoteOnAttenuation(n, a); this.call(`note_on_attenuation ${n} ${a}`); }
  previewNoteOff(n) { this.player.previewNoteOff(n); this.call(`note_off ${n}`); }
  previewAllNotesOff() { this.player.previewAllNotesOff(); this.call('all_notes_off'); }
  previewSetFmOpMask(m) { this.player.previewSetFmOpMask(m); this.call(`op_mask ${m}`); }
  previewSetMono(m) { this.player.previewSetMono(m); this.call(`mono ${m}`); }
}

function hex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

/** Replays the recorded script on the reference. Returns the differences. */
function compareWithReference() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'player-parity-'));
  const scriptPath = path.join(dir, 'script.txt');
  fs.writeFileSync(scriptPath, `${script.join('\n')}\n`);
  const run = spawnSync(reference, [scriptPath], { encoding: 'utf8', maxBuffer: 1 << 28 });
  fs.rmSync(dir, { recursive: true, force: true });
  if (run.status !== 0) return [`reference failed (${run.status ?? run.signal}): ${run.stderr}`];
  const lines = run.stdout.split('\n');
  const differences = [];
  const failed = new Set();
  expected.forEach(({ line, where }, i) => {
    const owner = where.split(':')[0];
    if (lines[i] !== line && !failed.has(owner)) {
      failed.add(owner);
      differences.push(`${where}: player "${line}", reference "${lines[i]}"`);
    }
  });
  return differences;
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

/** The song packed by the main module, or null with the compile error. */
function packSong(main, file) {
  const { text, dir } = mirrorSong(main.mod, file);
  const z0 = performance.now();
  if (main.compile(text, dir) !== 0) return { error: main.lastError().split('\n')[0] };
  const blob = main.pack();
  const packMs = performance.now() - z0;
  return blob ? { blob, packMs } : { error: main.lastError() };
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

function runSongs(main, playerModule, probeModule) {
  const results = { count: 0, skipped: [], errors: [], startErrors: [] };
  const stats = [];
  // One probe for every song in turn, as a page would use the player.
  const longProbe = probeModule ? instantiatePlayer(probeModule) : null;
  let previous = null;
  for (const file of songs) {
    const name = path.relative(process.cwd(), file);
    const packed = packSong(main, file);
    if (!packed.blob) {
      results.skipped.push(`${name}: ${packed.error}`);
      continue;
    }
    const { blob } = packed;
    results.count++;
    const s = new Session(playerModule, name);
    s.song(file, blob);
    const t0 = performance.now();
    const rc = s.load(0, 'start');
    const loadMs = performance.now() - t0;
    if (rc !== 0) {
      results.startErrors.push(`${name}: ${s.player.lastError()}`);
      previous = { file, blob };
      continue;
    }

    // From the start, then once it has ended.
    const played = s.play(blocksFor(seconds), 'start');
    s.render(BLOCK, BLOCK, 'start, after the end');
    if (s.player.lastError()) results.errors.push(`${name}: ${s.player.lastError()}`);

    // From a tick a third of the way in.
    s.load(0, 'seek');
    s.render(Math.floor(played / 3) * BLOCK, BLOCK, 'seek');
    const midTick = s.player.ticks() >>> 0;
    s.stop();
    s.load(midTick, `start at ${midTick}`);
    s.play(blocksFor(Math.min(seconds, 20)), `from tick ${midTick}`);
    s.render(BLOCK, BLOCK, `from tick ${midTick}, after the end`);

    // Relinked a third of the way in: to the same song, then to the
    // previous song.
    let relinkMs = 0;
    const relinkTo = [{ file, blob, label: 'itself' }];
    if (previous) relinkTo.push({ ...previous, label: 'the previous song' });
    for (const target of relinkTo) {
      s.song(file, blob);
      s.load(0, `before relink to ${target.label}`);
      s.play(Math.floor(played / 3), `before relink to ${target.label}`);
      s.song(target.file, target.blob);
      const r0 = performance.now();
      s.relink(`relink to ${target.label}`);
      relinkMs = Math.max(relinkMs, performance.now() - r0);
      s.play(blocksFor(Math.min(seconds, 20)), `relinked to ${target.label}`);
      s.render(BLOCK, BLOCK, `relinked to ${target.label}, after the end`);
    }

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
      printed: s.player.log.length + solo.log.length,
      blobBytes: blob.length,
      packMs: packed.packMs,
      loadMs: Math.min(loadMs, soloLoadMs),
      relinkMs,
      firstBlockMs: soloTimes[0] ?? 0,
      ...summarize(soloTimes.slice(1)),
      p99: soloTimes.length > 1 ? [...soloTimes.slice(1)].sort((a, b) => a - b)[Math.floor((soloTimes.length - 2) * 0.99)] : 0,
      heapPeak,
    });
    if (verbose) console.log(`played ${name} (${played} blocks, blob ${blob.length} B)`);
    previous = { file, blob };
  }
  const longPeak = longProbe ? longProbe.ex.probe_heap_base() + longProbe.ex.probe_heap_peak() : null;
  return { results, stats, longPeak };
}

// ---------------------------------------------------------------------------
// Preview scenarios
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

function runPreview(playerModule) {
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
  for (const [name, run] of runs) run(new Session(playerModule, `preview ${name}`));
  return runs.length;
}

/** Preview notes over a playing song, with mute masks and chip switches. */
function runSongWithPreview(main, playerModule, file) {
  const packed = packSong(main, file);
  if (!packed.blob) return `${path.basename(file)} did not compile: ${packed.error}`;
  const s = new Session(playerModule, `song with preview (${path.basename(file)})`);
  s.previewInit(SAMPLE_RATE);
  s.previewLoadFm(PATCH_A);
  s.song(file, packed.blob);
  s.load(0, 'start');
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
  s.stop();
  s.render(BLOCK * 100);
  s.previewInit(SAMPLE_RATE);
  s.load(0, 'restart');
  s.previewNoteOn(64, 100);
  s.render(BLOCK * 20);
  s.previewInit(SAMPLE_RATE);
  s.render(BLOCK * 20);
  return null;
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

const previewCount = runPreview(playerModule);
const { results, stats, longPeak } = runSongs(main, playerModule, probeModule);
const mixed = songs.find((f) => f.endsWith('song_blob_fm_psg.mml')) ?? songs[0];
const mixedError = runSongWithPreview(main, playerModule, mixed);
const differences = compareWithReference();

console.log(`compared ${expected.length} results of ${previewCount} preview scenarios, ` +
  `${results.count} songs and a song with preview with the reference: ` +
  `${differences.length ? `${differences.length} differ` : 'identical'}`);
for (const d of differences) console.log(`  FAIL ${d}`);
if (mixedError) console.log(`  FAIL ${mixedError}`);
for (const s of results.skipped) console.log(`  skipped ${s}`);
for (const e of results.startErrors) console.log(`  start error: ${e}`);
for (const e of results.errors) console.log(`  playback error: ${e}`);

if (stats.length) {
  const heaviest = [...stats].sort((a, b) => b.blobBytes - a.blobBytes).slice(0, 5);
  const slowest = [...stats].sort((a, b) => b.mean - a.mean).slice(0, 5);
  const slowestLoads = [...stats].sort((a, b) => b.loadMs - a.loadMs).slice(0, 5);
  const fmt = (s) => `  ${s.name}: blob ${s.blobBytes} B, compile and pack ${s.packMs.toFixed(2)} ms, ` +
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

process.exit(differences.length || mixedError ? 1 : 0);
