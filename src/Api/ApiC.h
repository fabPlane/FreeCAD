/* SPDX-License-Identifier: LGPL-2.1-or-later */

/*
 * The FreeCAD API as a C ABI: what the WebAssembly module exports, and what any in-process
 * host (a test, another language's FFI) can call. Same messages as the WebSocket and stdio
 * transports; see PROTOCOL.md.
 *
 *   fcapi_init(config)          start FreeCAD (once) and the dispatcher; 0 on success
 *   fcapi_dispatch(req, n, &m)  one request in, one reply out (malloc'd; fcapi_free it)
 *   fcapi_free(p)               release a reply
 *   fcapi_shutdown()            close documents, stop FreeCAD
 *   fcapi_last_error()          why the last call returned an error
 *
 * Events: in WebAssembly the module calls Module.__fcapiEvent(Uint8Array) with each encoded
 * event (the view is into the heap; copy it). Natively, register fcapi_set_event_callback.
 * Events a request raises are delivered before fcapi_dispatch returns.
 *
 * config is JSON (or NULL):
 *   {
 *     "argv0": "/path/to/FreeCADApiServer",  // where FreeCAD looks for its home
 *     "env": {"FREECAD_USER_HOME": "/home/freecad"},  // set before FreeCAD starts
 *     "modules": ["Part", "Sketcher", "PartDesign"],  // imported after start
 *     "preload": "/work/model.FCStd",                  // opened after start, or ""
 *     "python": true, "events": true,
 *     "eventEncoding": "cbor"                          // or "json"
 *   }
 */

#ifndef FREECAD_API_C_H
#define FREECAD_API_C_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
# define FCAPI_EXPORT __declspec(dllexport)
#else
# define FCAPI_EXPORT __attribute__((visibility("default")))
#endif

typedef void (*fcapi_event_callback)(const uint8_t* data, size_t size, void* user);

FCAPI_EXPORT int fcapi_init(const char* config_json);
FCAPI_EXPORT uint8_t* fcapi_dispatch(const uint8_t* request, size_t size, size_t* out_size);
FCAPI_EXPORT void fcapi_free(void* reply);
FCAPI_EXPORT void fcapi_shutdown(void);
FCAPI_EXPORT const char* fcapi_last_error(void);
FCAPI_EXPORT void fcapi_set_event_callback(fcapi_event_callback callback, void* user);

#ifdef __cplusplus
}
#endif

#endif /* FREECAD_API_C_H */
