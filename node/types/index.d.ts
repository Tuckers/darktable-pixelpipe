/**
 * @app/dtpipe — TypeScript declarations
 *
 * Node.js native addon wrapping libdtpipe, a headless darktable pixel pipeline.
 */

/**
 * Result of a render operation. The pixel data is in RGBA order, 8 bits per
 * channel, packed with no row padding (stride = width * 4).
 *
 * Call dispose() when done to release the underlying buffer.
 */
export declare class RenderResult {
  /** Raw RGBA pixel data (width × height × 4 bytes). */
  readonly buffer: ArrayBuffer;
  readonly width: number;
  readonly height: number;
  /**
   * Release the pixel buffer immediately rather than waiting for GC.
   * After dispose(), buffer returns undefined.
   */
  dispose(): void;
}

/**
 * A loaded RAW image. Owns the decoded sensor data.
 * Call dispose() when done to release native memory.
 */
export declare class Image {
  readonly width: number;
  readonly height: number;
  readonly cameraMaker: string | null;
  readonly cameraModel: string | null;
  /** Release the underlying native image handle. */
  dispose(): void;
}

/**
 * A processing pipeline attached to an Image.
 * Owns the pipeline state including module params and history.
 * Call dispose() when done to release native memory.
 */
export declare class Pipeline {
  // -------------------------------------------------------------------------
  // Parameter access
  // -------------------------------------------------------------------------

  /**
   * Set a named float parameter on a module.
   * @param module  Module operation name (e.g. "exposure").
   * @param param   Parameter name (e.g. "exposure").
   * @param value   New value.
   * @throws Error if the module or parameter is not found.
   */
  setParam(module: string, param: string, value: number): void;

  /**
   * Get a named float parameter from a module.
   * @param module  Module operation name.
   * @param param   Parameter name.
   * @returns Current value.
   * @throws Error if the module or parameter is not found.
   */
  getParam(module: string, param: string): number;

  /**
   * Enable or disable a module in the pipeline.
   * @param module   Module operation name.
   * @param enabled  True to enable, false to disable.
   * @throws Error if the module is not found.
   */
  enableModule(module: string, enabled: boolean): void;

  /**
   * Query whether a module is currently enabled.
   * @param module  Module operation name.
   * @returns True if enabled.
   * @throws Error if the module is not found.
   */
  isModuleEnabled(module: string): boolean;

  // -------------------------------------------------------------------------
  // Rendering
  // -------------------------------------------------------------------------

  /**
   * Render the full image at a given scale.
   * Runs off the main thread. The returned RenderResult owns its pixel buffer.
   * @param scale  Scale factor (e.g. 0.5 for half-size). Must be > 0.
   */
  render(scale: number): Promise<RenderResult>;

  /**
   * Render a rectangular region of the image at a given scale.
   * Runs off the main thread.
   * @param x      Left edge of the region in full-resolution pixels.
   * @param y      Top edge of the region in full-resolution pixels.
   * @param width  Width of the region in full-resolution pixels. Must be > 0.
   * @param height Height of the region in full-resolution pixels. Must be > 0.
   * @param scale  Scale factor applied to the region. Must be > 0.
   */
  renderRegion(
    x: number,
    y: number,
    width: number,
    height: number,
    scale: number,
  ): Promise<RenderResult>;

  // -------------------------------------------------------------------------
  // Export
  // -------------------------------------------------------------------------

  /**
   * Export the full-resolution image as a JPEG file.
   * Runs off the main thread.
   * @param path     Output file path.
   * @param quality  JPEG quality, 1–100. Defaults to 90.
   */
  exportJpeg(path: string, quality?: number): Promise<void>;

  /**
   * Export the full-resolution image as a PNG file.
   * Runs off the main thread.
   * @param path  Output file path.
   */
  exportPng(path: string): Promise<void>;

  /**
   * Export the full-resolution image as a TIFF file.
   * Runs off the main thread.
   * @param path  Output file path.
   * @param bits  Bit depth: 8, 16, or 32. Defaults to 16.
   */
  exportTiff(path: string, bits?: number): Promise<void>;

  // -------------------------------------------------------------------------
  // History / XMP
  // -------------------------------------------------------------------------

  /**
   * Serialize the current pipeline history (enabled state + params for all
   * modules) to a JSON string.
   * @returns JSON string in the dtpipe history format.
   * @throws Error on failure.
   */
  serializeHistory(): string;

  /**
   * Apply a previously serialized history to this pipeline.
   * @param json  JSON string produced by serializeHistory().
   * @throws Error on parse failure or invalid data.
   */
  loadHistory(json: string): void;

  /**
   * Load a darktable XMP sidecar file and apply its history to this pipeline.
   * @param path  Path to the .xmp file.
   * @throws Error if the file cannot be read or parsed.
   */
  loadXmp(path: string): void;

  /**
   * Write the current pipeline history as a darktable-compatible XMP sidecar.
   * @param path  Output .xmp file path.
   * @throws Error on write failure.
   */
  saveXmp(path: string): void;

  // -------------------------------------------------------------------------
  // Lifecycle
  // -------------------------------------------------------------------------

  /** Release the underlying native pipeline. Safe to call multiple times. */
  dispose(): void;
}

// ---------------------------------------------------------------------------
// Module-level functions
// ---------------------------------------------------------------------------

/**
 * Load a RAW image file from disk.
 * @param path  Absolute or relative path to the RAW file.
 * @returns A new Image object. Call image.dispose() when finished.
 * @throws Error on load failure (file not found, unsupported format, etc.).
 */
export declare function loadRaw(path: string): Image;

/**
 * Create a processing pipeline for the given image.
 * The pipeline borrows the image — keep the Image alive for the pipeline's
 * lifetime.
 * @param image  A loaded Image.
 * @returns A new Pipeline. Call pipeline.dispose() when finished.
 * @throws Error on failure.
 */
export declare function createPipeline(image: Image): Pipeline;
