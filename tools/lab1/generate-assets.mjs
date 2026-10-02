// Deterministic, valid PNG/OBJ fixtures consumed by the engine's real loaders.
// No runtime delay or repeated decode loop is used to simulate load.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';
import { deflateSync } from 'node:zlib';

const args = process.argv.slice(2);
const option = (name, fallback) => {
  const index = args.indexOf(name);
  return index < 0 ? fallback : args[index + 1];
};
const output = path.resolve(option('--output', 'C:/tge/lab1-assets'));
const size = Number(option('--texture-size', 2048));
const grid = Number(option('--mesh-grid', 256));
if (!Number.isInteger(size) || size < 16 || size > 4096 ||
    !Number.isInteger(grid) || grid < 4 || grid > 512) {
  throw new Error('texture-size must be 16..4096; mesh-grid must be 4..512.');
}
const spec = { version: 1, seed: 19790605, textures: 12, textureSize: size, meshes: 4, meshGrid: grid };
const manifestPath = path.join(output, 'manifest.json');
const digest = file => crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
if (fs.existsSync(output) && fs.readdirSync(output).length !== 0) {
  if (!fs.existsSync(manifestPath)) throw new Error(`Refusing to overwrite nonempty ${output}. Choose a new output directory.`);
  const old = JSON.parse(fs.readFileSync(manifestPath, 'utf8'));
  if (JSON.stringify(old.spec) !== JSON.stringify(spec)) throw new Error('Existing fixture has different parameters. Choose a new output directory.');
  for (const file of old.files) {
    if (digest(path.join(output, file.name)) !== file.sha256) throw new Error(`Fixture modified: ${file.name}`);
  }
  console.log(`Verified existing fixture: ${output}`);
  process.exit(0);
}
fs.mkdirSync(output, { recursive: true });
const crcTable = new Uint32Array(256);
for (let n = 0; n < 256; ++n) {
  let c = n;
  for (let k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
  crcTable[n] = c >>> 0;
}
function pngChunk(type, data) {
  const content = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  let crc = 0xffffffff;
  for (const byte of content) crc = crcTable[(crc ^ byte) & 255] ^ (crc >>> 8);
  const result = Buffer.alloc(content.length + 8);
  result.writeUInt32BE(data.length, 0);
  content.copy(result, 4);
  result.writeUInt32BE((crc ^ 0xffffffff) >>> 0, result.length - 4);
  return result;
}
function texture(index) {
  const stride = size * 4 + 1;
  const raw = Buffer.alloc(stride * size);
  let seed = (spec.seed + index * 7919) >>> 0;
  for (let y = 0; y < size; ++y) {
    for (let x = 0; x < size; ++x) {
      seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
      const offset = y * stride + 1 + x * 4;
      // Distinct material-like grain with a visible colored checker pattern.
      const tile = ((x >>> 6) ^ (y >>> 6)) & 1;
      raw[offset] = (((seed >>> 0) & 63) + 48 + tile * 96 + index * 7) & 255;
      raw[offset + 1] = (((seed >>> 8) & 63) + 48 + (1 - tile) * 96 + index * 11) & 255;
      raw[offset + 2] = (((seed >>> 16) & 127) + 32 + index * 13) & 255;
      raw[offset + 3] = 255;
    }
  }
  const header = Buffer.alloc(13);
  header.writeUInt32BE(size, 0); header.writeUInt32BE(size, 4);
  header[8] = 8; header[9] = 6;
  return Buffer.concat([
    Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]),
    pngChunk('IHDR', header), pngChunk('IDAT', deflateSync(raw, { level: 6 })), pngChunk('IEND', Buffer.alloc(0)),
  ]);
}
function mesh(file, index) {
  const fd = fs.openSync(file, 'wx');
  let buffer = `# Deterministic terrain patch ${index}; ${grid} x ${grid} cells\no terrain_${index}\n`;
  const append = text => {
    buffer += text;
    if (buffer.length > 1024 * 1024) { fs.writeSync(fd, buffer); buffer = ''; }
  };
  try {
    for (let y = 0; y <= grid; ++y) for (let x = 0; x <= grid; ++x) {
      const px = (x / grid - 0.5) * 2;
      const pz = (y / grid - 0.5) * 2;
      const py = Math.sin(px * (4 + index)) * Math.cos(pz * (5 + index)) * 0.15;
      append(`v ${px.toFixed(6)} ${py.toFixed(6)} ${pz.toFixed(6)}\n`);
    }
    for (let y = 0; y <= grid; ++y) for (let x = 0; x <= grid; ++x) append(`vt ${(x / grid).toFixed(6)} ${(y / grid).toFixed(6)}\n`);
    for (let y = 0; y < grid; ++y) for (let x = 0; x < grid; ++x) {
      const a = y * (grid + 1) + x + 1, b = a + 1, c = a + grid + 1, d = c + 1;
      append(`f ${a}/${a} ${c}/${c} ${b}/${b}\nf ${b}/${b} ${c}/${c} ${d}/${d}\n`);
    }
    if (buffer) fs.writeSync(fd, buffer);
  } finally { fs.closeSync(fd); }
}
const files = [];
function record(name, type, properties) {
  const file = path.join(output, name);
  files.push({ name, type, ...properties, bytes: fs.statSync(file).size, sha256: digest(file) });
  console.log(`${name}: ${files.at(-1).bytes} bytes`);
}
for (let i = 0; i < spec.textures; ++i) {
  const name = `texture_${String(i).padStart(2, '0')}.png`;
  fs.writeFileSync(path.join(output, name), texture(i), { flag: 'wx' });
  record(name, 'texture', { width: size, height: size, channels: 4 });
}
for (let i = 0; i < spec.meshes; ++i) {
  const name = `mesh_${String(i).padStart(2, '0')}.obj`;
  mesh(path.join(output, name), i);
  record(name, 'mesh', { vertices: (grid + 1) ** 2, triangles: grid * grid * 2 });
}
fs.writeFileSync(manifestPath, JSON.stringify({ spec, files }, null, 2) + '\n', { flag: 'wx' });
console.log(`Ready: ${output}`);
