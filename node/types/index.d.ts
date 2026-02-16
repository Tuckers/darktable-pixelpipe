/**
 * @app/dtpipe — TypeScript declarations
 */

/**
 * A loaded RAW image. Call dispose() when done to release native memory.
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
 * Load a RAW image file.
 * @param path Absolute or relative path to the RAW file.
 * @throws Error on load failure.
 */
export declare function loadRaw(path: string): Image;
