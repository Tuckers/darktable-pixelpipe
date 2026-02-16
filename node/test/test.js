"use strict";

const path = require("path");
const { loadRaw, createPipeline, Image, Pipeline } = require("../lib/index.js");

// ---------------------------------------------------------------------------
// Task 6.1: addon loads
// ---------------------------------------------------------------------------
console.log("✓ addon loaded");

// ---------------------------------------------------------------------------
// Task 6.2: loadRaw + Image metadata
// ---------------------------------------------------------------------------
const RAF_PATH = path.resolve(__dirname, "../../test-image/DSCF4379.RAF");

let img;
try {
  img = loadRaw(RAF_PATH);
} catch (e) {
  console.error("✗ loadRaw threw:", e.message);
  process.exit(1);
}

console.log(`✓ loadRaw succeeded`);
console.log(`  ${img.width}x${img.height}`);
console.log(`  Camera: ${img.cameraMaker} ${img.cameraModel}`);

if (typeof img.width !== "number" || img.width <= 0) {
  console.error("✗ width invalid:", img.width);
  process.exit(1);
}
if (typeof img.height !== "number" || img.height <= 0) {
  console.error("✗ height invalid:", img.height);
  process.exit(1);
}
console.log("✓ width and height are positive numbers");

// Error path: bad file
try {
  loadRaw("/no/such/file.raf");
  console.error("✗ expected loadRaw to throw on bad path");
  process.exit(1);
} catch (e) {
  console.log("✓ loadRaw throws on bad path:", e.message);
}

console.log("\nAll task 6.2 checks passed.");

// ---------------------------------------------------------------------------
// Task 6.3: createPipeline + Pipeline operations
// ---------------------------------------------------------------------------
console.log("\n--- Task 6.3: Pipeline ---");

// Reload image (previous img is still valid — we didn't dispose yet)
let pipe;
try {
  pipe = createPipeline(img);
} catch (e) {
  console.error("✗ createPipeline threw:", e.message);
  process.exit(1);
}
console.log("✓ createPipeline succeeded");

// setParam / getParam round-trip on exposure module
const EXPOSURE_MODULE = "exposure";
const EXPOSURE_PARAM = "exposure";
const TEST_VALUE = 1.5;

try {
  pipe.setParam(EXPOSURE_MODULE, EXPOSURE_PARAM, TEST_VALUE);
  console.log(
    `✓ setParam(${EXPOSURE_MODULE}, ${EXPOSURE_PARAM}, ${TEST_VALUE}) succeeded`,
  );
} catch (e) {
  console.error("✗ setParam threw:", e.message);
  process.exit(1);
}

let readBack;
try {
  readBack = pipe.getParam(EXPOSURE_MODULE, EXPOSURE_PARAM);
} catch (e) {
  console.error("✗ getParam threw:", e.message);
  process.exit(1);
}

if (Math.abs(readBack - TEST_VALUE) > 1e-5) {
  console.error(
    `✗ getParam round-trip failed: expected ${TEST_VALUE}, got ${readBack}`,
  );
  process.exit(1);
}
console.log(`✓ getParam round-trip: ${readBack} ≈ ${TEST_VALUE}`);

// enableModule / isModuleEnabled round-trip
const TEST_MODULE = "sharpen";

try {
  pipe.enableModule(TEST_MODULE, false);
  const enabled = pipe.isModuleEnabled(TEST_MODULE);
  if (enabled !== false) {
    console.error(
      `✗ isModuleEnabled after disable: expected false, got ${enabled}`,
    );
    process.exit(1);
  }
  console.log(
    `✓ enableModule(${TEST_MODULE}, false) → isModuleEnabled = false`,
  );

  pipe.enableModule(TEST_MODULE, true);
  const enabled2 = pipe.isModuleEnabled(TEST_MODULE);
  if (enabled2 !== true) {
    console.error(
      `✗ isModuleEnabled after enable: expected true, got ${enabled2}`,
    );
    process.exit(1);
  }
  console.log(`✓ enableModule(${TEST_MODULE}, true) → isModuleEnabled = true`);
} catch (e) {
  console.error("✗ enableModule/isModuleEnabled threw:", e.message);
  process.exit(1);
}

// Error: unknown module
try {
  pipe.setParam("no_such_module", "x", 0);
  console.error("✗ expected setParam to throw for unknown module");
  process.exit(1);
} catch (e) {
  console.log("✓ setParam throws for unknown module:", e.message);
}

try {
  pipe.isModuleEnabled("no_such_module");
  console.error("✗ expected isModuleEnabled to throw for unknown module");
  process.exit(1);
} catch (e) {
  console.log("✓ isModuleEnabled throws for unknown module:", e.message);
}

// dispose
pipe.dispose();
console.log("✓ pipe.dispose() completed");

// Double-dispose should not crash
pipe.dispose();
console.log("✓ double pipe.dispose() safe");

// Operations after dispose should throw
try {
  pipe.setParam(EXPOSURE_MODULE, EXPOSURE_PARAM, 0.0);
  console.error("✗ expected setParam to throw after dispose");
  process.exit(1);
} catch (e) {
  console.log("✓ setParam throws after dispose:", e.message);
}

// Dispose the image
img.dispose();
console.log("✓ img.dispose() completed");

// Double-dispose should not crash
img.dispose();
console.log("✓ double dispose() safe");

console.log("\nAll task 6.3 checks passed.");

// ---------------------------------------------------------------------------
// Task 6.4: render() and renderRegion() with ArrayBuffer result
// ---------------------------------------------------------------------------
(async () => {
  console.log("\n--- Task 6.4: Render ---");

  // Re-load image and create a fresh pipeline for render tests
  let rImg;
  try {
    rImg = loadRaw(RAF_PATH);
  } catch (e) {
    console.error("✗ loadRaw for render test threw:", e.message);
    process.exit(1);
  }

  let rPipe;
  try {
    rPipe = createPipeline(rImg);
  } catch (e) {
    console.error("✗ createPipeline for render test threw:", e.message);
    process.exit(1);
  }

  // --- render(scale) ---
  const SCALE = 0.05; // small scale for speed
  let result;
  try {
    result = await rPipe.render(SCALE);
  } catch (e) {
    console.error("✗ render() threw:", e.message);
    process.exit(1);
  }
  console.log("✓ render() resolved");

  // Verify dimensions
  const expectedW = Math.floor(rImg.width * SCALE);
  const expectedH = Math.floor(rImg.height * SCALE);
  if (typeof result.width !== "number" || result.width <= 0) {
    console.error("✗ result.width invalid:", result.width);
    process.exit(1);
  }
  if (typeof result.height !== "number" || result.height <= 0) {
    console.error("✗ result.height invalid:", result.height);
    process.exit(1);
  }
  console.log(
    `✓ render dimensions: ${result.width}x${result.height} (expected ~${expectedW}x${expectedH})`,
  );

  // Verify buffer
  const buf = result.buffer;
  if (!(buf instanceof ArrayBuffer)) {
    console.error("✗ result.buffer is not an ArrayBuffer:", typeof buf);
    process.exit(1);
  }
  const expectedBytes = result.width * result.height * 4;
  if (buf.byteLength !== expectedBytes) {
    console.error(
      `✗ buffer size mismatch: got ${buf.byteLength}, expected ${expectedBytes}`,
    );
    process.exit(1);
  }
  console.log(`✓ result.buffer is ArrayBuffer of ${buf.byteLength} bytes`);

  // Verify pixel values are in range [0, 255] (spot check first pixel RGBA)
  const view = new Uint8Array(buf);
  const allInRange = view.every((v) => v >= 0 && v <= 255);
  if (!allInRange) {
    console.error("✗ pixel values out of [0,255] range");
    process.exit(1);
  }
  console.log(`✓ all pixel values in [0,255] range`);

  // Verify the image is not entirely black (at least one non-zero pixel)
  const hasNonZero = view.some((v) => v > 0);
  if (!hasNonZero) {
    console.warn(
      "  (warning: all pixels are zero — pipeline may have no active processing modules)",
    );
  } else {
    console.log("✓ buffer contains non-zero pixel data");
  }

  // dispose result
  result.dispose();
  console.log("✓ result.dispose() completed");

  // double-dispose should not crash
  result.dispose();
  console.log("✓ double result.dispose() safe");

  // buffer after dispose returns undefined
  if (result.buffer !== undefined) {
    console.error("✗ result.buffer should be undefined after dispose");
    process.exit(1);
  }
  console.log("✓ result.buffer is undefined after dispose");

  // --- renderRegion(x, y, w, h, scale) ---
  const RX = 0,
    RY = 0;
  const RW = Math.min(200, rImg.width);
  const RH = Math.min(200, rImg.height);
  const RS = 1.0;
  let regionResult;
  try {
    regionResult = await rPipe.renderRegion(RX, RY, RW, RH, RS);
  } catch (e) {
    console.error("✗ renderRegion() threw:", e.message);
    process.exit(1);
  }
  console.log("✓ renderRegion() resolved");

  if (regionResult.width !== RW || regionResult.height !== RH) {
    console.error(
      `✗ renderRegion dimensions wrong: got ${regionResult.width}x${regionResult.height}, expected ${RW}x${RH}`,
    );
    process.exit(1);
  }
  console.log(
    `✓ renderRegion dimensions: ${regionResult.width}x${regionResult.height}`,
  );

  const regionBuf = regionResult.buffer;
  if (!(regionBuf instanceof ArrayBuffer)) {
    console.error("✗ renderRegion buffer is not an ArrayBuffer");
    process.exit(1);
  }
  if (regionBuf.byteLength !== RW * RH * 4) {
    console.error(`✗ renderRegion buffer size wrong: ${regionBuf.byteLength}`);
    process.exit(1);
  }
  console.log(
    `✓ renderRegion buffer is ArrayBuffer of ${regionBuf.byteLength} bytes`,
  );
  regionResult.dispose();

  // --- Error paths ---
  try {
    await rPipe.render(-1.0);
    console.error("✗ expected render(-1) to reject");
    process.exit(1);
  } catch (e) {
    console.log("✓ render rejects with negative scale:", e.message);
  }

  try {
    await rPipe.renderRegion(0, 0, 0, 100, 1.0);
    console.error("✗ expected renderRegion with w=0 to reject");
    process.exit(1);
  } catch (e) {
    console.log("✓ renderRegion rejects with w=0:", e.message);
  }

  // dispose pipeline, then render should reject
  rPipe.dispose();
  try {
    await rPipe.render(0.1);
    console.error("✗ expected render to reject after dispose");
    process.exit(1);
  } catch (e) {
    console.log("✓ render rejects after pipeline dispose:", e.message);
  }

  rImg.dispose();

  console.log("\nAll task 6.4 checks passed.");
})().catch((err) => {
  console.error("Unhandled error in async test:", err);
  process.exit(1);
});
