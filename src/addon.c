/* addon.c — N-API wrapper (v11) around the v10 pipeline (src/main.c).
 *
 * One function: runAll(inputBuffer) -> outputBuffer.
 * The pipeline (alloc / run_all / out_ptr / out_len) is reused unchanged
 * from main.c; the wrapper only moves bytes between JS Buffers and the
 * pipeline's static buffers. Only N-API v1 functions are used, so the
 * addon loads on Node 22 and Node 26 alike.
 */
#include <node_api.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* Pipeline entry points (defined in src/main.c). */
uint8_t *alloc(size_t size);
const uint8_t *out_ptr(void);
size_t out_len(void);
void run_all(void);

static napi_value RunAll(napi_env env, napi_callback_info info) {
    size_t argc = 1;
    napi_value args[1];
    napi_status st = napi_get_cb_info(env, info, &argc, args, NULL, NULL);
    if (st != napi_ok || argc < 1) {
        napi_throw_type_error(env, NULL, "runAll(buffer) expected");
        return NULL;
    }
    void *data = NULL;
    size_t len = 0;
    st = napi_get_buffer_info(env, args[0], &data, &len);
    if (st != napi_ok) {
        napi_throw_type_error(env, NULL, "runAll argument must be a Buffer");
        return NULL;
    }
    /* Copy the input into the pipeline's static input buffer. */
    uint8_t *in = alloc(len);
    memcpy(in, data, len);
    run_all();
    /* Copy the output into a fresh Buffer (the pipeline's static output
     * buffer is reused on the next call, so the JS side must own a copy). */
    napi_value out = NULL;
    st = napi_create_buffer_copy(env, out_len(), out_ptr(), NULL, &out);
    if (st != napi_ok) {
        napi_throw_error(env, NULL, "failed to create output Buffer");
        return NULL;
    }
    return out;
}

NAPI_MODULE_INIT() {
    napi_value fn = NULL;
    napi_status st = napi_create_function(env, NULL, 0, RunAll, NULL, &fn);
    if (st != napi_ok) return NULL;
    st = napi_set_named_property(env, exports, "runAll", fn);
    if (st != napi_ok) return NULL;
    return exports;
}
