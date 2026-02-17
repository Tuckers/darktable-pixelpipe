/**
 * Task 7.4: Node.js addon tests (TypeScript)
 *
 * Uses the built-in node:test runner (Node >= 18) and node:assert/strict.
 * Run via:  npm run test:ts
 * Type-check only: npm run typecheck
 */

/* eslint-disable @typescript-eslint/no-require-imports */
// Explicit type annotations on assert are required for TS assertion function narrowing (TS2775)
const {
  describe,
  it,
  before,
  after,
}: typeof import("node:test") = require("node:test");
const assert: typeof import("node:assert/strict") = require("node:assert/strict");
const fs: typeof import("node:fs") = require("node:fs");
const os: typeof import("node:os") = require("node:os");
const path: typeof import("node:path") = require("node:path");

import type { Image, Pipeline, RenderResult } from "../types/index.d.ts";

const dtpipe =
  require("../lib/index.js") as typeof import("../types/index.d.ts");
const { loadRaw, createPipeline } = dtpipe;

const RAF_PATH = path.resolve(__dirname, "../../test-image/DSCF4379.RAF");

// ---------------------------------------------------------------------------
// 1. Module loading
// ---------------------------------------------------------------------------
describe("module loading", () => {
  it("can require @app/dtpipe", () => {
    assert.ok(dtpipe, "module is truthy");
    assert.equal(typeof loadRaw, "function");
    assert.equal(typeof createPipeline, "function");
  });
});

// ---------------------------------------------------------------------------
// 2. Image loading
// ---------------------------------------------------------------------------
describe("image loading", () => {
  it("loadRaw() returns an Image with correct metadata", () => {
    const img: Image = loadRaw(RAF_PATH);
    assert.ok(img, "Image is truthy");
    assert.equal(typeof img.width, "number");
    assert.equal(typeof img.height, "number");
    assert.ok(img.width > 0, "width > 0");
    assert.ok(img.height > 0, "height > 0");
    assert.equal(typeof img.cameraMaker, "string");
    assert.equal(typeof img.cameraModel, "string");
    img.dispose();
  });

  it("loadRaw() throws on invalid file path", () => {
    assert.throws(
      () => loadRaw("/no/such/file.raf"),
      (err: unknown) => {
        assert.ok(err instanceof Error);
        return true;
      },
    );
  });

  it("dispose() is safe to call multiple times", () => {
    const img: Image = loadRaw(RAF_PATH);
    img.dispose();
    assert.doesNotThrow(() => img.dispose());
  });
});

// ---------------------------------------------------------------------------
// 3. Pipeline operations
// ---------------------------------------------------------------------------
describe("pipeline operations", () => {
  let img: Image;
  let pipe: Pipeline;

  before(() => {
    img = loadRaw(RAF_PATH);
    pipe = createPipeline(img);
  });

  after(() => {
    pipe.dispose();
    img.dispose();
  });

  it("createPipeline() returns a Pipeline", () => {
    assert.ok(pipe, "Pipeline is truthy");
  });

  it("setParam / getParam round-trip (exposure)", () => {
    pipe.setParam("exposure", "exposure", 1.5);
    const val = pipe.getParam("exposure", "exposure");
    assert.ok(Math.abs(val - 1.5) < 1e-5, `expected 1.5, got ${val}`);
  });

  it("setParam / getParam accepts negative values", () => {
    pipe.setParam("exposure", "exposure", -0.5);
    const val = pipe.getParam("exposure", "exposure");
    assert.ok(Math.abs(val - -0.5) < 1e-5, `expected -0.5, got ${val}`);
  });

  it("enableModule / isModuleEnabled round-trip", () => {
    pipe.enableModule("sharpen", false);
    assert.equal(pipe.isModuleEnabled("sharpen"), false);
    pipe.enableModule("sharpen", true);
    assert.equal(pipe.isModuleEnabled("sharpen"), true);
  });

  it("setParam throws for unknown module", () => {
    assert.throws(() => pipe.setParam("no_such_module", "x", 0), Error);
  });

  it("setParam throws for unknown param", () => {
    assert.throws(() => pipe.setParam("exposure", "no_such_param", 0), Error);
  });

  it("isModuleEnabled throws for unknown module", () => {
    assert.throws(() => pipe.isModuleEnabled("no_such_module"), Error);
  });

  it("enableModule throws for unknown module", () => {
    assert.throws(() => pipe.enableModule("no_such_module", true), Error);
  });

  it("methods throw after dispose", () => {
    const p2 = createPipeline(img);
    p2.dispose();
    assert.throws(() => p2.setParam("exposure", "exposure", 0), Error);
    assert.throws(() => p2.getParam("exposure", "exposure"), Error);
    assert.throws(() => p2.enableModule("exposure", true), Error);
    assert.throws(() => p2.isModuleEnabled("exposure"), Error);
  });

  it("dispose() is safe to call multiple times", () => {
    const p2 = createPipeline(img);
    p2.dispose();
    assert.doesNotThrow(() => p2.dispose());
  });
});

// ---------------------------------------------------------------------------
// 4. Rendering
// ---------------------------------------------------------------------------
describe("rendering", () => {
  let img: Image;
  let pipe: Pipeline;

  before(() => {
    img = loadRaw(RAF_PATH);
    pipe = createPipeline(img);
  });

  after(() => {
    pipe.dispose();
    img.dispose();
  });

  it("render() resolves to a RenderResult with correct shape", async () => {
    const result: RenderResult = await pipe.render(0.05);
    assert.ok(result.width > 0, "width > 0");
    assert.ok(result.height > 0, "height > 0");
    assert.ok(result.buffer instanceof ArrayBuffer, "buffer is ArrayBuffer");
    assert.equal(result.buffer.byteLength, result.width * result.height * 4);
    result.dispose();
  });

  it("render() pixel values are in [0, 255]", async () => {
    const result: RenderResult = await pipe.render(0.05);
    const view = new Uint8Array(result.buffer);
    assert.ok(
      view.every((v) => v >= 0 && v <= 255),
      "all pixels in [0,255]",
    );
    result.dispose();
  });

  it("render() at 2x scale produces ~2x dimensions", async () => {
    const r1: RenderResult = await pipe.render(0.05);
    const r2: RenderResult = await pipe.render(0.1);
    const wRatio = r2.width / r1.width;
    const hRatio = r2.height / r1.height;
    assert.ok(wRatio > 1.5 && wRatio < 2.5, `width ratio ${wRatio} not ~2`);
    assert.ok(hRatio > 1.5 && hRatio < 2.5, `height ratio ${hRatio} not ~2`);
    r1.dispose();
    r2.dispose();
  });

  it("renderRegion() returns correct dimensions", async () => {
    const W = 200,
      H = 150;
    const result: RenderResult = await pipe.renderRegion(0, 0, W, H, 1.0);
    assert.equal(result.width, W);
    assert.equal(result.height, H);
    assert.equal(result.buffer.byteLength, W * H * 4);
    result.dispose();
  });

  it("renderRegion() buffer is ArrayBuffer", async () => {
    const result: RenderResult = await pipe.renderRegion(0, 0, 100, 100, 1.0);
    assert.ok(result.buffer instanceof ArrayBuffer);
    result.dispose();
  });

  it("RenderResult.dispose() is safe to call multiple times", async () => {
    const result: RenderResult = await pipe.render(0.05);
    result.dispose();
    assert.doesNotThrow(() => result.dispose());
  });

  it("buffer is undefined after dispose", async () => {
    const result: RenderResult = await pipe.render(0.05);
    result.dispose();
    // After dispose the native backing is freed; buffer getter returns undefined
    assert.equal(result.buffer as unknown, undefined);
  });

  it("render() rejects with negative scale", async () => {
    await assert.rejects(() => pipe.render(-1.0), Error);
  });

  it("render() rejects with zero scale", async () => {
    await assert.rejects(() => pipe.render(0), Error);
  });

  it("renderRegion() rejects with zero width", async () => {
    await assert.rejects(() => pipe.renderRegion(0, 0, 0, 100, 1.0), Error);
  });

  it("renderRegion() rejects with zero height", async () => {
    await assert.rejects(() => pipe.renderRegion(0, 0, 100, 0, 1.0), Error);
  });

  it("renderRegion() rejects with negative scale", async () => {
    await assert.rejects(() => pipe.renderRegion(0, 0, 100, 100, -1.0), Error);
  });

  it("render() rejects after pipeline dispose", async () => {
    const p2 = createPipeline(img);
    p2.dispose();
    await assert.rejects(() => p2.render(0.1), Error);
  });
});

// ---------------------------------------------------------------------------
// 5. Export
// ---------------------------------------------------------------------------
describe("export", () => {
  let img: Image;
  let pipe: Pipeline;
  let tmpDir: string;

  before(() => {
    img = loadRaw(RAF_PATH);
    pipe = createPipeline(img);
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "dtpipe-test-"));
  });

  after(() => {
    pipe.dispose();
    img.dispose();
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it("exportJpeg() creates a non-empty file", async () => {
    const out = path.join(tmpDir, "out.jpg");
    await pipe.exportJpeg(out, 85);
    assert.ok(fs.existsSync(out));
    assert.ok(fs.statSync(out).size > 1000);
  });

  it("exportJpeg() with default quality succeeds", async () => {
    const out = path.join(tmpDir, "out_def.jpg");
    await pipe.exportJpeg(out);
    assert.ok(fs.existsSync(out));
  });

  it("exportJpeg() rejects with quality=0", async () => {
    await assert.rejects(
      () => pipe.exportJpeg(path.join(tmpDir, "q0.jpg"), 0),
      Error,
    );
  });

  it("exportJpeg() rejects with quality=101", async () => {
    await assert.rejects(
      () => pipe.exportJpeg(path.join(tmpDir, "q101.jpg"), 101),
      Error,
    );
  });

  it("exportPng() creates a non-empty file", async () => {
    const out = path.join(tmpDir, "out.png");
    await pipe.exportPng(out);
    assert.ok(fs.existsSync(out));
    assert.ok(fs.statSync(out).size > 1000);
  });

  it("exportTiff(bits=8) creates a non-empty file", async () => {
    const out = path.join(tmpDir, "out8.tiff");
    await pipe.exportTiff(out, 8);
    assert.ok(fs.existsSync(out));
    assert.ok(fs.statSync(out).size > 1000);
  });

  it("exportTiff(bits=16) creates a non-empty file", async () => {
    const out = path.join(tmpDir, "out16.tiff");
    await pipe.exportTiff(out, 16);
    assert.ok(fs.existsSync(out));
    assert.ok(fs.statSync(out).size > 1000);
  });

  it("exportTiff() with default bits succeeds", async () => {
    const out = path.join(tmpDir, "out_def.tiff");
    await pipe.exportTiff(out);
    assert.ok(fs.existsSync(out));
  });

  it("exportTiff() rejects with invalid bits", async () => {
    await assert.rejects(
      () => pipe.exportTiff(path.join(tmpDir, "bad.tiff"), 24),
      Error,
    );
  });

  it("export methods reject after pipeline dispose", async () => {
    const p2 = createPipeline(img);
    p2.dispose();
    await assert.rejects(
      () => p2.exportJpeg(path.join(tmpDir, "d.jpg")),
      Error,
    );
    await assert.rejects(() => p2.exportPng(path.join(tmpDir, "d.png")), Error);
    await assert.rejects(
      () => p2.exportTiff(path.join(tmpDir, "d.tiff")),
      Error,
    );
  });
});

// ---------------------------------------------------------------------------
// 6. History
// ---------------------------------------------------------------------------
describe("history", () => {
  let img: Image;
  let pipe: Pipeline;
  let tmpDir: string;

  before(() => {
    img = loadRaw(RAF_PATH);
    pipe = createPipeline(img);
    tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "dtpipe-hist-"));
  });

  after(() => {
    pipe.dispose();
    img.dispose();
    fs.rmSync(tmpDir, { recursive: true, force: true });
  });

  it("serializeHistory() returns valid JSON", () => {
    const json = pipe.serializeHistory();
    assert.equal(typeof json, "string");
    assert.ok(json.length > 0);
    const parsed = JSON.parse(json);
    assert.ok(parsed);
  });

  it("serializeHistory() JSON contains a modules key", () => {
    const parsed = JSON.parse(pipe.serializeHistory());
    assert.ok("modules" in parsed, 'JSON has "modules" key');
  });

  it("loadHistory() round-trip preserves float param", () => {
    pipe.setParam("exposure", "exposure", 2.0);
    const json = pipe.serializeHistory();
    pipe.setParam("exposure", "exposure", 0.0);
    pipe.loadHistory(json);
    const val = pipe.getParam("exposure", "exposure");
    assert.ok(Math.abs(val - 2.0) < 1e-4, `expected 2.0, got ${val}`);
  });

  it("loadHistory() round-trip preserves enabled state", () => {
    pipe.enableModule("sharpen", false);
    const json = pipe.serializeHistory();
    pipe.enableModule("sharpen", true);
    pipe.loadHistory(json);
    assert.equal(pipe.isModuleEnabled("sharpen"), false);
    pipe.enableModule("sharpen", true); // restore
  });

  it("loadHistory() throws on malformed JSON", () => {
    assert.throws(() => pipe.loadHistory("not json {{"), Error);
  });

  it("loadHistory() throws on empty string", () => {
    assert.throws(() => pipe.loadHistory(""), Error);
  });

  it("saveXmp() creates a valid darktable XMP file", () => {
    const xmpPath = path.join(tmpDir, "out.xmp");
    pipe.saveXmp(xmpPath);
    assert.ok(fs.existsSync(xmpPath));
    const content = fs.readFileSync(xmpPath, "utf8");
    assert.ok(content.includes("xmpmeta"));
    assert.ok(content.includes("darktable"));
  });

  it("loadXmp() can round-trip through saveXmp()", () => {
    pipe.setParam("exposure", "exposure", 1.25);
    const xmpPath = path.join(tmpDir, "roundtrip.xmp");
    pipe.saveXmp(xmpPath);
    pipe.setParam("exposure", "exposure", 0.0);
    pipe.loadXmp(xmpPath);
    const val = pipe.getParam("exposure", "exposure");
    assert.ok(Math.abs(val - 1.25) < 1e-3, `expected 1.25, got ${val}`);
  });

  it("loadXmp() throws on missing file", () => {
    assert.throws(() => pipe.loadXmp("/no/such/file.xmp"), Error);
  });

  it("history methods throw after dispose", () => {
    const p2 = createPipeline(img);
    p2.dispose();
    assert.throws(() => p2.serializeHistory(), Error);
    assert.throws(() => p2.loadHistory("{}"), Error);
    assert.throws(() => p2.saveXmp(path.join(tmpDir, "d.xmp")), Error);
    assert.throws(() => p2.loadXmp(path.join(tmpDir, "d.xmp")), Error);
  });
});
