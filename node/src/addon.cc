/*
 * addon.cc - Node.js N-API addon for libdtpipe
 */

#include <napi.h>
#include <cstring>
#include <string>
#include "dtpipe.h"

// ---------------------------------------------------------------------------
// Instance data — holds constructor references for Image, Pipeline, RenderResult
// ---------------------------------------------------------------------------

struct AddonData {
    Napi::FunctionReference imageCtor;
    Napi::FunctionReference pipelineCtor;
    Napi::FunctionReference renderResultCtor;
};

// ---------------------------------------------------------------------------
// RenderResult class
// ---------------------------------------------------------------------------

class RenderResult : public Napi::ObjectWrap<RenderResult> {
public:
    static Napi::Function GetClass(Napi::Env env) {
        return DefineClass(env, "RenderResult", {
            InstanceAccessor<&RenderResult::GetBuffer>("buffer"),
            InstanceAccessor<&RenderResult::GetWidth>("width"),
            InstanceAccessor<&RenderResult::GetHeight>("height"),
            InstanceMethod<&RenderResult::Dispose>("dispose"),
        });
    }

    RenderResult(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<RenderResult>(info),
          width_(0), height_(0), disposed_(false) {}

    ~RenderResult() {}

    // Populate from a completed render. The pixel data is copied into a
    // SharedArrayBuffer so the caller can free the dt_render_result_t
    // immediately after this call.
    void SetResult(dt_render_result_t *result, Napi::Env env) {
        if (!result) return;

        width_  = result->width;
        height_ = result->height;

        // Allocate a packed pixel buffer and copy from the render result.
        // stride may be > width*4; we pack tightly (width*4 per row) for
        // simpler consumption on the JS side.
        // The buffer is wrapped in an ArrayBuffer with an external finalizer,
        // so V8 owns it after this point and will free it when GC'd.
        size_t packed_stride = (size_t)result->width * 4;
        size_t byte_len      = packed_stride * (size_t)result->height;

        unsigned char *pixels = static_cast<unsigned char *>(malloc(byte_len));
        if (pixels) {
            // Copy rows, stripping any row padding
            for (int row = 0; row < result->height; ++row) {
                const unsigned char *src =
                    result->pixels + (size_t)row * (size_t)result->stride;
                memcpy(pixels + row * packed_stride, src, packed_stride);
            }
            // ArrayBuffer with external data — finalizer frees the buffer
            Napi::ArrayBuffer ab = Napi::ArrayBuffer::New(
                env, pixels, byte_len,
                [](Napi::BasicEnv /*e*/, void *data) {
                    free(data);
                });
            sab_ref_ = Napi::Persistent(ab.As<Napi::Object>());
        }
    }

private:
    int    width_;
    int    height_;
    bool   disposed_;
    // Persistent reference to the ArrayBuffer JS value
    Napi::Reference<Napi::Object> sab_ref_;

    void MarkDisposed() {
        if (!disposed_) {
            disposed_ = true;
            sab_ref_.Reset();
        }
    }

    Napi::Value GetBuffer(const Napi::CallbackInfo &info) {
        if (disposed_ || sab_ref_.IsEmpty()) return info.Env().Undefined();
        return sab_ref_.Value();
    }

    Napi::Value GetWidth(const Napi::CallbackInfo &info) {
        if (disposed_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), width_);
    }

    Napi::Value GetHeight(const Napi::CallbackInfo &info) {
        if (disposed_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), height_);
    }

    Napi::Value Dispose(const Napi::CallbackInfo &info) {
        MarkDisposed();
        return info.Env().Undefined();
    }
};

// ---------------------------------------------------------------------------
// Image class
// ---------------------------------------------------------------------------

class Image : public Napi::ObjectWrap<Image> {
public:
    static Napi::Function GetClass(Napi::Env env) {
        return DefineClass(env, "Image", {
            InstanceAccessor<&Image::GetWidth>("width"),
            InstanceAccessor<&Image::GetHeight>("height"),
            InstanceAccessor<&Image::GetCameraMaker>("cameraMaker"),
            InstanceAccessor<&Image::GetCameraModel>("cameraModel"),
            InstanceMethod<&Image::Dispose>("dispose"),
        });
    }

    // Called from JS via new Image() — not intended for direct use.
    // Real construction happens through loadRaw().
    Image(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<Image>(info), img_(nullptr) {}

    ~Image() { FreeImage(); }

    // Internal helper used by loadRaw
    void SetImage(dt_image_t *img) { img_ = img; }

    dt_image_t *GetNative() const { return img_; }

private:
    dt_image_t *img_;

    void FreeImage() {
        if (img_) {
            dtpipe_free_image(img_);
            img_ = nullptr;
        }
    }

    Napi::Value GetWidth(const Napi::CallbackInfo &info) {
        if (!img_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), dtpipe_get_width(img_));
    }

    Napi::Value GetHeight(const Napi::CallbackInfo &info) {
        if (!img_) return info.Env().Undefined();
        return Napi::Number::New(info.Env(), dtpipe_get_height(img_));
    }

    Napi::Value GetCameraMaker(const Napi::CallbackInfo &info) {
        if (!img_) return info.Env().Undefined();
        const char *val = dtpipe_get_camera_maker(img_);
        if (!val) return info.Env().Null();
        return Napi::String::New(info.Env(), val);
    }

    Napi::Value GetCameraModel(const Napi::CallbackInfo &info) {
        if (!img_) return info.Env().Undefined();
        const char *val = dtpipe_get_camera_model(img_);
        if (!val) return info.Env().Null();
        return Napi::String::New(info.Env(), val);
    }

    Napi::Value Dispose(const Napi::CallbackInfo &info) {
        FreeImage();
        return info.Env().Undefined();
    }
};

// ---------------------------------------------------------------------------
// Async workers for render
// ---------------------------------------------------------------------------

class RenderWorker : public Napi::AsyncWorker {
public:
    RenderWorker(Napi::Promise::Deferred deferred,
                 dt_pipe_t *pipe,
                 float scale,
                 Napi::Reference<Napi::Object> renderResultCtorRef)
        : Napi::AsyncWorker(deferred.Env()),
          deferred_(deferred),
          pipe_(pipe),
          scale_(scale),
          result_(nullptr),
          renderResultCtorRef_(std::move(renderResultCtorRef)) {}

    void Execute() override {
        result_ = dtpipe_render(pipe_, scale_);
        if (!result_) {
            const char *err = dtpipe_get_last_error();
            SetError(err && err[0] ? err : "dtpipe_render failed");
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        Napi::Object obj = renderResultCtorRef_.Value().As<Napi::Function>().New({});
        RenderResult *wrapper = Napi::ObjectWrap<RenderResult>::Unwrap(obj);
        wrapper->SetResult(result_, env);
        dtpipe_free_render(result_);
        result_ = nullptr;
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error &e) override {
        if (result_) {
            dtpipe_free_render(result_);
            result_ = nullptr;
        }
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    dt_pipe_t *pipe_;
    float scale_;
    dt_render_result_t *result_;
    Napi::Reference<Napi::Object> renderResultCtorRef_;
};

class RenderRegionWorker : public Napi::AsyncWorker {
public:
    RenderRegionWorker(Napi::Promise::Deferred deferred,
                       dt_pipe_t *pipe,
                       int x, int y, int w, int h, float scale,
                       Napi::Reference<Napi::Object> renderResultCtorRef)
        : Napi::AsyncWorker(deferred.Env()),
          deferred_(deferred),
          pipe_(pipe),
          x_(x), y_(y), w_(w), h_(h), scale_(scale),
          result_(nullptr),
          renderResultCtorRef_(std::move(renderResultCtorRef)) {}

    void Execute() override {
        result_ = dtpipe_render_region(pipe_, x_, y_, w_, h_, scale_);
        if (!result_) {
            const char *err = dtpipe_get_last_error();
            SetError(err && err[0] ? err : "dtpipe_render_region failed");
        }
    }

    void OnOK() override {
        Napi::Env env = Env();
        Napi::Object obj = renderResultCtorRef_.Value().As<Napi::Function>().New({});
        RenderResult *wrapper = Napi::ObjectWrap<RenderResult>::Unwrap(obj);
        wrapper->SetResult(result_, env);
        dtpipe_free_render(result_);
        result_ = nullptr;
        deferred_.Resolve(obj);
    }

    void OnError(const Napi::Error &e) override {
        if (result_) {
            dtpipe_free_render(result_);
            result_ = nullptr;
        }
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    dt_pipe_t *pipe_;
    int x_, y_, w_, h_;
    float scale_;
    dt_render_result_t *result_;
    Napi::Reference<Napi::Object> renderResultCtorRef_;
};

// ---------------------------------------------------------------------------
// Async workers for export
// ---------------------------------------------------------------------------

class ExportJpegWorker : public Napi::AsyncWorker {
public:
    ExportJpegWorker(Napi::Promise::Deferred deferred,
                     dt_pipe_t *pipe,
                     std::string path,
                     int quality)
        : Napi::AsyncWorker(deferred.Env()),
          deferred_(deferred),
          pipe_(pipe),
          path_(std::move(path)),
          quality_(quality) {}

    void Execute() override {
        int rc = dtpipe_export_jpeg(pipe_, path_.c_str(), quality_);
        if (rc != DTPIPE_OK) {
            const char *err = dtpipe_get_last_error();
            std::string msg = "exportJpeg failed (rc=" + std::to_string(rc) + ")";
            if (err && err[0]) { msg += ": "; msg += err; }
            SetError(msg);
        }
    }

    void OnOK() override {
        deferred_.Resolve(Env().Undefined());
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    dt_pipe_t *pipe_;
    std::string path_;
    int quality_;
};

class ExportPngWorker : public Napi::AsyncWorker {
public:
    ExportPngWorker(Napi::Promise::Deferred deferred,
                    dt_pipe_t *pipe,
                    std::string path)
        : Napi::AsyncWorker(deferred.Env()),
          deferred_(deferred),
          pipe_(pipe),
          path_(std::move(path)) {}

    void Execute() override {
        int rc = dtpipe_export_png(pipe_, path_.c_str());
        if (rc != DTPIPE_OK) {
            const char *err = dtpipe_get_last_error();
            std::string msg = "exportPng failed (rc=" + std::to_string(rc) + ")";
            if (err && err[0]) { msg += ": "; msg += err; }
            SetError(msg);
        }
    }

    void OnOK() override {
        deferred_.Resolve(Env().Undefined());
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    dt_pipe_t *pipe_;
    std::string path_;
};

class ExportTiffWorker : public Napi::AsyncWorker {
public:
    ExportTiffWorker(Napi::Promise::Deferred deferred,
                     dt_pipe_t *pipe,
                     std::string path,
                     int bits)
        : Napi::AsyncWorker(deferred.Env()),
          deferred_(deferred),
          pipe_(pipe),
          path_(std::move(path)),
          bits_(bits) {}

    void Execute() override {
        int rc = dtpipe_export_tiff(pipe_, path_.c_str(), bits_);
        if (rc != DTPIPE_OK) {
            const char *err = dtpipe_get_last_error();
            std::string msg = "exportTiff failed (rc=" + std::to_string(rc) + ")";
            if (err && err[0]) { msg += ": "; msg += err; }
            SetError(msg);
        }
    }

    void OnOK() override {
        deferred_.Resolve(Env().Undefined());
    }

    void OnError(const Napi::Error &e) override {
        deferred_.Reject(e.Value());
    }

private:
    Napi::Promise::Deferred deferred_;
    dt_pipe_t *pipe_;
    std::string path_;
    int bits_;
};

// ---------------------------------------------------------------------------
// Pipeline class
// ---------------------------------------------------------------------------

class Pipeline : public Napi::ObjectWrap<Pipeline> {
public:
    static Napi::Function GetClass(Napi::Env env) {
        return DefineClass(env, "Pipeline", {
            InstanceMethod<&Pipeline::SetParam>("setParam"),
            InstanceMethod<&Pipeline::GetParam>("getParam"),
            InstanceMethod<&Pipeline::EnableModule>("enableModule"),
            InstanceMethod<&Pipeline::IsModuleEnabled>("isModuleEnabled"),
            InstanceMethod<&Pipeline::Render>("render"),
            InstanceMethod<&Pipeline::RenderRegion>("renderRegion"),
            InstanceMethod<&Pipeline::ExportJpeg>("exportJpeg"),
            InstanceMethod<&Pipeline::ExportPng>("exportPng"),
            InstanceMethod<&Pipeline::ExportTiff>("exportTiff"),
            InstanceMethod<&Pipeline::SerializeHistory>("serializeHistory"),
            InstanceMethod<&Pipeline::LoadHistory>("loadHistory"),
            InstanceMethod<&Pipeline::LoadXmp>("loadXmp"),
            InstanceMethod<&Pipeline::SaveXmp>("saveXmp"),
            InstanceMethod<&Pipeline::Dispose>("dispose"),
        });
    }

    // Not for direct JS construction — use createPipeline().
    Pipeline(const Napi::CallbackInfo &info)
        : Napi::ObjectWrap<Pipeline>(info), pipe_(nullptr) {}

    ~Pipeline() { FreePipe(); }

    void SetPipe(dt_pipe_t *pipe) { pipe_ = pipe; }

private:
    dt_pipe_t *pipe_;

    void FreePipe() {
        if (pipe_) {
            dtpipe_free(pipe_);
            pipe_ = nullptr;
        }
    }

    // setParam(module: string, param: string, value: number): void
    Napi::Value SetParam(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 3 || !info[0].IsString() || !info[1].IsString() || !info[2].IsNumber()) {
            Napi::TypeError::New(env, "setParam(module: string, param: string, value: number)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string mod   = info[0].As<Napi::String>().Utf8Value();
        std::string param = info[1].As<Napi::String>().Utf8Value();
        float value       = info[2].As<Napi::Number>().FloatValue();

        int rc = dtpipe_set_param_float(pipe_, mod.c_str(), param.c_str(), value);
        if (rc != DTPIPE_OK) {
            std::string msg = "setParam failed (rc=" + std::to_string(rc) + ")";
            const char *e = dtpipe_get_last_error();
            if (e && e[0]) { msg += ": "; msg += e; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    // getParam(module: string, param: string): number
    Napi::Value GetParam(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsString()) {
            Napi::TypeError::New(env, "getParam(module: string, param: string)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string mod   = info[0].As<Napi::String>().Utf8Value();
        std::string param = info[1].As<Napi::String>().Utf8Value();

        float out = 0.0f;
        int rc = dtpipe_get_param_float(pipe_, mod.c_str(), param.c_str(), &out);
        if (rc != DTPIPE_OK) {
            std::string msg = "getParam failed (rc=" + std::to_string(rc) + ")";
            const char *e = dtpipe_get_last_error();
            if (e && e[0]) { msg += ": "; msg += e; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        return Napi::Number::New(env, out);
    }

    // enableModule(module: string, enabled: boolean): void
    Napi::Value EnableModule(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 2 || !info[0].IsString() || !info[1].IsBoolean()) {
            Napi::TypeError::New(env, "enableModule(module: string, enabled: boolean)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string mod = info[0].As<Napi::String>().Utf8Value();
        bool enabled    = info[1].As<Napi::Boolean>().Value();

        int rc = dtpipe_enable_module(pipe_, mod.c_str(), enabled ? 1 : 0);
        if (rc != DTPIPE_OK) {
            std::string msg = "enableModule failed (rc=" + std::to_string(rc) + ")";
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    // isModuleEnabled(module: string): boolean
    Napi::Value IsModuleEnabled(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "isModuleEnabled(module: string)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string mod = info[0].As<Napi::String>().Utf8Value();

        int enabled = 0;
        int rc = dtpipe_is_module_enabled(pipe_, mod.c_str(), &enabled);
        if (rc != DTPIPE_OK) {
            std::string msg = "isModuleEnabled failed (rc=" + std::to_string(rc) + ")";
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        return Napi::Boolean::New(env, enabled != 0);
    }

    // render(scale: number): Promise<RenderResult>
    Napi::Value Render(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        auto deferred = Napi::Promise::Deferred::New(env);

        if (!pipe_) {
            deferred.Reject(Napi::Error::New(env, "Pipeline already disposed").Value());
            return deferred.Promise();
        }
        if (info.Length() < 1 || !info[0].IsNumber()) {
            deferred.Reject(Napi::TypeError::New(env, "render(scale: number)").Value());
            return deferred.Promise();
        }
        float scale = info[0].As<Napi::Number>().FloatValue();
        if (scale <= 0.0f) {
            deferred.Reject(Napi::RangeError::New(env, "scale must be > 0").Value());
            return deferred.Promise();
        }

        AddonData *data = env.GetInstanceData<AddonData>();
        auto ctorRef = Napi::Persistent(data->renderResultCtor.Value().As<Napi::Object>());

        auto *worker = new RenderWorker(deferred, pipe_, scale, std::move(ctorRef));
        worker->Queue();
        return deferred.Promise();
    }

    // renderRegion(x, y, width, height, scale): Promise<RenderResult>
    Napi::Value RenderRegion(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        auto deferred = Napi::Promise::Deferred::New(env);

        if (!pipe_) {
            deferred.Reject(Napi::Error::New(env, "Pipeline already disposed").Value());
            return deferred.Promise();
        }
        if (info.Length() < 5 ||
            !info[0].IsNumber() || !info[1].IsNumber() ||
            !info[2].IsNumber() || !info[3].IsNumber() || !info[4].IsNumber()) {
            deferred.Reject(Napi::TypeError::New(
                env, "renderRegion(x: number, y: number, width: number, height: number, scale: number)").Value());
            return deferred.Promise();
        }

        int x     = info[0].As<Napi::Number>().Int32Value();
        int y     = info[1].As<Napi::Number>().Int32Value();
        int w     = info[2].As<Napi::Number>().Int32Value();
        int h     = info[3].As<Napi::Number>().Int32Value();
        float scale = info[4].As<Napi::Number>().FloatValue();

        if (w <= 0 || h <= 0) {
            deferred.Reject(Napi::RangeError::New(env, "width and height must be > 0").Value());
            return deferred.Promise();
        }
        if (scale <= 0.0f) {
            deferred.Reject(Napi::RangeError::New(env, "scale must be > 0").Value());
            return deferred.Promise();
        }

        AddonData *data = env.GetInstanceData<AddonData>();
        auto ctorRef = Napi::Persistent(data->renderResultCtor.Value().As<Napi::Object>());

        auto *worker = new RenderRegionWorker(deferred, pipe_, x, y, w, h, scale, std::move(ctorRef));
        worker->Queue();
        return deferred.Promise();
    }

    // exportJpeg(path: string, quality?: number): Promise<void>
    Napi::Value ExportJpeg(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        auto deferred = Napi::Promise::Deferred::New(env);

        if (!pipe_) {
            deferred.Reject(Napi::Error::New(env, "Pipeline already disposed").Value());
            return deferred.Promise();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            deferred.Reject(Napi::TypeError::New(env, "exportJpeg(path: string, quality?: number)").Value());
            return deferred.Promise();
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();
        int quality = 90;
        if (info.Length() >= 2 && info[1].IsNumber()) {
            quality = info[1].As<Napi::Number>().Int32Value();
        }
        if (quality < 1 || quality > 100) {
            deferred.Reject(Napi::RangeError::New(env, "quality must be 1-100").Value());
            return deferred.Promise();
        }

        auto *worker = new ExportJpegWorker(deferred, pipe_, std::move(path), quality);
        worker->Queue();
        return deferred.Promise();
    }

    // exportPng(path: string): Promise<void>
    Napi::Value ExportPng(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        auto deferred = Napi::Promise::Deferred::New(env);

        if (!pipe_) {
            deferred.Reject(Napi::Error::New(env, "Pipeline already disposed").Value());
            return deferred.Promise();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            deferred.Reject(Napi::TypeError::New(env, "exportPng(path: string)").Value());
            return deferred.Promise();
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();

        auto *worker = new ExportPngWorker(deferred, pipe_, std::move(path));
        worker->Queue();
        return deferred.Promise();
    }

    // exportTiff(path: string, bits?: number): Promise<void>
    Napi::Value ExportTiff(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        auto deferred = Napi::Promise::Deferred::New(env);

        if (!pipe_) {
            deferred.Reject(Napi::Error::New(env, "Pipeline already disposed").Value());
            return deferred.Promise();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            deferred.Reject(Napi::TypeError::New(env, "exportTiff(path: string, bits?: number)").Value());
            return deferred.Promise();
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();
        int bits = 16;
        if (info.Length() >= 2 && info[1].IsNumber()) {
            bits = info[1].As<Napi::Number>().Int32Value();
        }
        if (bits != 8 && bits != 16 && bits != 32) {
            deferred.Reject(Napi::RangeError::New(env, "bits must be 8, 16, or 32").Value());
            return deferred.Promise();
        }

        auto *worker = new ExportTiffWorker(deferred, pipe_, std::move(path), bits);
        worker->Queue();
        return deferred.Promise();
    }

    // serializeHistory(): string
    Napi::Value SerializeHistory(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        char *json = dtpipe_serialize_history(pipe_);
        if (!json) {
            const char *err = dtpipe_get_last_error();
            std::string msg = "serializeHistory failed";
            if (err && err[0]) { msg += ": "; msg += err; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
            return env.Undefined();
        }
        Napi::String result = Napi::String::New(env, json);
        free(json);
        return result;
    }

    // loadHistory(json: string): void
    Napi::Value LoadHistory(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "loadHistory(json: string)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string json = info[0].As<Napi::String>().Utf8Value();
        int rc = dtpipe_load_history(pipe_, json.c_str());
        if (rc != DTPIPE_OK) {
            std::string msg = "loadHistory failed (rc=" + std::to_string(rc) + ")";
            const char *err = dtpipe_get_last_error();
            if (err && err[0]) { msg += ": "; msg += err; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    // loadXmp(path: string): void
    Napi::Value LoadXmp(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "loadXmp(path: string)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();
        int rc = dtpipe_load_xmp(pipe_, path.c_str());
        if (rc != DTPIPE_OK) {
            std::string msg = "loadXmp failed (rc=" + std::to_string(rc) + ")";
            const char *err = dtpipe_get_last_error();
            if (err && err[0]) { msg += ": "; msg += err; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    // saveXmp(path: string): void
    Napi::Value SaveXmp(const Napi::CallbackInfo &info) {
        Napi::Env env = info.Env();
        if (!pipe_) {
            Napi::Error::New(env, "Pipeline already disposed").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        if (info.Length() < 1 || !info[0].IsString()) {
            Napi::TypeError::New(env, "saveXmp(path: string)").ThrowAsJavaScriptException();
            return env.Undefined();
        }
        std::string path = info[0].As<Napi::String>().Utf8Value();
        int rc = dtpipe_save_xmp(pipe_, path.c_str());
        if (rc != DTPIPE_OK) {
            std::string msg = "saveXmp failed (rc=" + std::to_string(rc) + ")";
            const char *err = dtpipe_get_last_error();
            if (err && err[0]) { msg += ": "; msg += err; }
            Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        }
        return env.Undefined();
    }

    Napi::Value Dispose(const Napi::CallbackInfo &info) {
        FreePipe();
        return info.Env().Undefined();
    }
};

// ---------------------------------------------------------------------------
// loadRaw(path: string): Image
// ---------------------------------------------------------------------------

static Napi::Value LoadRaw(const Napi::CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsString()) {
        Napi::TypeError::New(env, "loadRaw expects a string path").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    std::string path = info[0].As<Napi::String>().Utf8Value();

    dt_image_t *img = dtpipe_load_raw(path.c_str());
    if (!img) {
        const char *err = dtpipe_get_last_error();
        std::string msg = "loadRaw failed";
        if (err && err[0]) { msg += ": "; msg += err; }
        Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        return env.Undefined();
    }

    AddonData *data = env.GetInstanceData<AddonData>();
    Napi::Object obj = data->imageCtor.New({});
    Image *wrapper = Napi::ObjectWrap<Image>::Unwrap(obj);
    wrapper->SetImage(img);
    return obj;
}

// ---------------------------------------------------------------------------
// createPipeline(image: Image): Pipeline
// ---------------------------------------------------------------------------

static Napi::Value CreatePipeline(const Napi::CallbackInfo &info) {
    Napi::Env env = info.Env();

    if (info.Length() < 1 || !info[0].IsObject()) {
        Napi::TypeError::New(env, "createPipeline expects an Image argument").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    Image *imgWrapper = Napi::ObjectWrap<Image>::Unwrap(info[0].As<Napi::Object>());
    if (!imgWrapper) {
        Napi::TypeError::New(env, "createPipeline: argument is not an Image").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    dt_image_t *img = imgWrapper->GetNative();
    if (!img) {
        Napi::Error::New(env, "createPipeline: Image has been disposed").ThrowAsJavaScriptException();
        return env.Undefined();
    }

    dt_pipe_t *pipe = dtpipe_create(img);
    if (!pipe) {
        const char *err = dtpipe_get_last_error();
        std::string msg = "createPipeline failed";
        if (err && err[0]) { msg += ": "; msg += err; }
        Napi::Error::New(env, msg).ThrowAsJavaScriptException();
        return env.Undefined();
    }

    AddonData *data = env.GetInstanceData<AddonData>();
    Napi::Object obj = data->pipelineCtor.New({});
    Pipeline *wrapper = Napi::ObjectWrap<Pipeline>::Unwrap(obj);
    wrapper->SetPipe(pipe);
    return obj;
}

// ---------------------------------------------------------------------------
// Module init
// ---------------------------------------------------------------------------

Napi::Object Init(Napi::Env env, Napi::Object exports) {
    dtpipe_init(nullptr);

    Napi::Function imageCtor        = Image::GetClass(env);
    Napi::Function pipelineCtor     = Pipeline::GetClass(env);
    Napi::Function renderResultCtor = RenderResult::GetClass(env);

    auto *data = new AddonData();
    data->imageCtor        = Napi::Persistent(imageCtor);
    data->pipelineCtor     = Napi::Persistent(pipelineCtor);
    data->renderResultCtor = Napi::Persistent(renderResultCtor);
    env.SetInstanceData(data);

    exports.Set("Image",          imageCtor);
    exports.Set("Pipeline",       pipelineCtor);
    exports.Set("RenderResult",   renderResultCtor);
    exports.Set("loadRaw",        Napi::Function::New(env, LoadRaw));
    exports.Set("createPipeline", Napi::Function::New(env, CreatePipeline));
    return exports;
}

NODE_API_MODULE(dtpipe, Init)
