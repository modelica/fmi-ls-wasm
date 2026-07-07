/*
 * adder-res-c-fmu — FMI 3.0 Co-Simulation FMU in pure C with resource access.
 *
 * Variables
 * ---------
 *  VR 0  time      independent  Float64  Independent time variable
 *  VR 1  input_a   input        Float64  First addend
 *  VR 2  input_b   input        Float64  Second addend
 *  VR 3  sum       output       Float64  sum = input_a + input_b + offset
 *
 * At instantiation, `offset` is read from resources/offset.txt:
 *
 *   __wasm__   Uses the resource-dir-callbacks imported interface.
 *              read-file("offset.txt") returns the raw file bytes.
 *
 *   (else)     Uses the FMI 3.0 `resourcePath` argument together with
 *              fopen to read <resourcePath>/offset.txt from the host
 *              filesystem.
 *
 * Two API modes, selected at compile-time via preprocessor:
 *
 *   __wasm__   (defined automatically by the WASI SDK / wasm32-wasip2 toolchain)
 *       Uses wit-bindgen-generated C bindings (co_simulation_fmu.h).
 *       Compiled to a WebAssembly component FMU binary.
 *
 *   (else, native toolchain — GCC / Clang targeting x86_64-linux, etc.)
 *       Uses the standard FMI 3.0 C API from fmi3Functions.h.
 *       Compiled to a shared library (.so) FMU binary.
 *
 * The core instance state struct, calculation helpers, and state-serialisation
 * utilities are shared between both paths via the ADDER_INSTANCE_FIELDS macro
 * and adder_impl_* static functions.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Common instance state fields
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Shared fields for every adder-res instance regardless of API path.
 * Expand inside each path's struct definition below.
 */
#define ADDER_INSTANCE_FIELDS \
    bool   in_initialization_mode; \
    double current_time;           \
    double input_a;                \
    double input_b;                \
    double offset;                 \
    double output_sum

/* ── Value-reference assignments ─────────────────────────────────────────── */
#define VR_TIME       0u
#define VR_INPUT_A    1u
#define VR_INPUT_B    2u
#define VR_OUTPUT_SUM 3u

/* ── Version string ──────────────────────────────────────────────────────── */
static const char VERSION_STRING[] = "3.0";

/* ══════════════════════════════════════════════════════════════════════════
 * API-specific struct definition and header includes
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef __wasm__

/* ── WASM / WIT-bindgen path ─────────────────────────────────────────────
 *
 * The generated header forward-declares this struct as an opaque type; we
 * complete it here before including the header so it resolves properly.
 */
struct exports_fmi_fmi3_co_simulation_co_simulation_instance_t {
    ADDER_INSTANCE_FIELDS;
};

#include "co_simulation_fmu.h"

/* Convenience alias used by the common adder_impl_* helpers. */
typedef exports_fmi_fmi3_co_simulation_co_simulation_instance_t adder_instance_t;

#else /* native FMI 3.0 C API ───────────────────────────────────────────── */

/* Include the standard FMI 3.0 headers (provides fmi3InstanceEnvironment,
 * callback typedefs, and all fmi3* function-type macros). */
#include "../fmi-standard/headers/fmi3Functions.h"

/* Instance struct for the native FMI 3.0 path.
 * Extends the common fields with the host-environment context passed at
 * instantiation time so that the FMU can call back into the host. */
typedef struct {
    ADDER_INSTANCE_FIELDS;
    fmi3InstanceEnvironment        instance_env;
    fmi3LogMessageCallback         log_cb;
    fmi3IntermediateUpdateCallback intermediate_update_cb;
} adder_instance_t;

#endif /* __wasm__ */

/* ══════════════════════════════════════════════════════════════════════════
 * Common state-serialisation helpers
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * State layout (41 bytes):
 *   [0]      : in_initialization_mode (1 = true)
 *   [1..8]   : current_time  (f64 little-endian)
 *   [9..16]  : input_a       (f64 little-endian)
 *   [17..24] : input_b       (f64 little-endian)
 *   [25..32] : offset        (f64 little-endian)
 *   [33..40] : output_sum    (f64 little-endian)
 */
#define ADDER_STATE_SIZE (1 + 5 * 8)

static void write_f64_le(uint8_t *dst, double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof(bits));
    dst[0] = (uint8_t)(bits);
    dst[1] = (uint8_t)(bits >> 8);
    dst[2] = (uint8_t)(bits >> 16);
    dst[3] = (uint8_t)(bits >> 24);
    dst[4] = (uint8_t)(bits >> 32);
    dst[5] = (uint8_t)(bits >> 40);
    dst[6] = (uint8_t)(bits >> 48);
    dst[7] = (uint8_t)(bits >> 56);
}

static double read_f64_le(const uint8_t *src)
{
    uint64_t bits =
        (uint64_t)src[0]         |
        ((uint64_t)src[1] << 8)  |
        ((uint64_t)src[2] << 16) |
        ((uint64_t)src[3] << 24) |
        ((uint64_t)src[4] << 32) |
        ((uint64_t)src[5] << 40) |
        ((uint64_t)src[6] << 48) |
        ((uint64_t)src[7] << 56);
    double v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

/* ══════════════════════════════════════════════════════════════════════════
 * Common adder_impl_* helpers (used by both API glue sections)
 * ══════════════════════════════════════════════════════════════════════════ */

/*
 * Read Float64 variables into caller-supplied buffer `out` (length n).
 * Returns true on success, false if any value reference is unknown.
 */
static bool adder_impl_get_float64(
    const adder_instance_t *inst,
    const uint32_t *vrs, size_t n,
    double *out)
{
    for (size_t i = 0; i < n; i++) {
        switch (vrs[i]) {
            case VR_TIME:       out[i] = inst->current_time; break;
            case VR_INPUT_A:    out[i] = inst->input_a;      break;
            case VR_INPUT_B:    out[i] = inst->input_b;      break;
            case VR_OUTPUT_SUM: out[i] = inst->output_sum;   break;
            default: return false;
        }
    }
    return true;
}

/*
 * Write Float64 variables from caller-supplied buffer `vals` (length n).
 * Returns true on success, false if any value reference is unknown or read-only.
 * Recomputes output_sum when called during initialisation.
 */
static bool adder_impl_set_float64(
    adder_instance_t *inst,
    const uint32_t *vrs, size_t n,
    const double *vals)
{
    for (size_t i = 0; i < n; i++) {
        switch (vrs[i]) {
            case VR_INPUT_A: inst->input_a = vals[i]; break;
            case VR_INPUT_B: inst->input_b = vals[i]; break;
            default: return false;
        }
    }
    if (inst->in_initialization_mode) {
        inst->output_sum = inst->input_a + inst->input_b + inst->offset;
    }
    return true;
}

/*
 * Advance time by one communication step and recompute output.
 */
static void adder_impl_do_step(
    adder_instance_t *inst,
    double current_communication_point,
    double communication_step_size)
{
    inst->current_time = current_communication_point + communication_step_size;
    inst->output_sum   = inst->input_a + inst->input_b + inst->offset;
}

/*
 * Serialise instance state into `buf` (must be at least ADDER_STATE_SIZE bytes).
 */
static void adder_impl_serialize_state(const adder_instance_t *inst, uint8_t *buf)
{
    buf[0] = inst->in_initialization_mode ? 1u : 0u;
    write_f64_le(buf + 1,  inst->current_time);
    write_f64_le(buf + 9,  inst->input_a);
    write_f64_le(buf + 17, inst->input_b);
    write_f64_le(buf + 25, inst->offset);
    write_f64_le(buf + 33, inst->output_sum);
}

/*
 * Restore instance state from `buf` (must be at least ADDER_STATE_SIZE bytes).
 */
static void adder_impl_deserialize_state(adder_instance_t *inst, const uint8_t *buf)
{
    inst->in_initialization_mode = (buf[0] != 0);
    inst->current_time = read_f64_le(buf + 1);
    inst->input_a      = read_f64_le(buf + 9);
    inst->input_b      = read_f64_le(buf + 17);
    inst->offset       = read_f64_le(buf + 25);
    inst->output_sum   = read_f64_le(buf + 33);
}

/*
 * Compute directional derivative: only VR_OUTPUT_SUM is a supported unknown.
 * sensitivity = sum of seed[i] for each known that is an input variable.
 * Writes result into out[0].  Returns true on success.
 */
static bool adder_impl_get_dir_deriv(
    const uint32_t *unknowns, size_t n_unknowns,
    const uint32_t *knowns,  size_t n_knowns,
    const double   *seed,    size_t n_seed,
    double         *out)
{
    if (n_unknowns != 1 || unknowns[0] != VR_OUTPUT_SUM) return false;
    double sensitivity = 0.0;
    for (size_t i = 0; i < n_knowns; i++) {
        uint32_t vr = knowns[i];
        if (vr == VR_INPUT_A || vr == VR_INPUT_B) {
            sensitivity += (i < n_seed) ? seed[i] : 0.0;
        }
    }
    out[0] = sensitivity;
    return true;
}

/*
 * Compute adjoint derivative: only VR_OUTPUT_SUM is a supported unknown.
 * For each known variable, sensitivity[i] = seed[0] if it is an input, else 0.
 * Returns true on success.
 */
static bool adder_impl_get_adj_deriv(
    const uint32_t *unknowns, size_t n_unknowns,
    const uint32_t *knowns,  size_t n_knowns,
    const double   *seed,    size_t n_seed,
    double         *out)
{
    if (n_unknowns != 1 || unknowns[0] != VR_OUTPUT_SUM) return false;
    double s = (n_seed > 0) ? seed[0] : 0.0;
    for (size_t i = 0; i < n_knowns; i++) {
        uint32_t vr = knowns[i];
        out[i] = (vr == VR_INPUT_A || vr == VR_INPUT_B) ? s : 0.0;
    }
    return true;
}

/* ══════════════════════════════════════════════════════════════════════════
 * API glue: WASM / WIT-bindgen path
 * ══════════════════════════════════════════════════════════════════════════ */

#ifdef __wasm__

/* ── Resource offset helper ──────────────────────────────────────────────── */

/*
 * Ask the host for the contents of resources/offset.txt via the optional
 * fmi:fmi3/resource-dir-callbacks import, parse the ASCII decimal value
 * and return it.  Returns 0.0 on any error (file not found, parse failure).
 */
static double read_resource_offset(void)
{
    co_simulation_fmu_string_t path;
    path.ptr = (uint8_t *)"offset.txt";
    path.len = 10; /* strlen("offset.txt") */

    co_simulation_fmu_list_u8_t ok_data;
    co_simulation_fmu_string_t  err_str;
    memset(&ok_data,  0, sizeof(ok_data));
    memset(&err_str,  0, sizeof(err_str));

    if (!fmi_fmi3_resource_dir_callbacks_read_file(&path, &ok_data, &err_str)) {
        /* err_str is set on failure — free it and return default */
        co_simulation_fmu_string_free(&err_str);
        return 0.0;
    }

    /* Parse the bytes as ASCII decimal; null-terminate for sscanf. */
    double offset = 0.0;
    if (ok_data.len > 0) {
        char *buf = (char *)malloc(ok_data.len + 1);
        if (buf) {
            memcpy(buf, ok_data.ptr, ok_data.len);
            buf[ok_data.len] = '\0';
            sscanf(buf, "%lf", &offset);
            free(buf);
        }
    }

    co_simulation_fmu_list_u8_free(&ok_data);
    return offset;
}

/* ── common interface ────────────────────────────────────────────────────── */

void exports_fmi_fmi3_common_get_version(co_simulation_fmu_string_t *ret)
{
    size_t len = sizeof(VERSION_STRING) - 1;
    uint8_t *buf = (uint8_t *)malloc(len);
    memcpy(buf, VERSION_STRING, len);
    ret->ptr = buf;
    ret->len = len;
}

/* ── co-simulation interface — resource lifecycle ────────────────────────── */

bool exports_fmi_fmi3_co_simulation_static_co_simulation_instance_instantiate_co_simulation(
    co_simulation_fmu_string_t  *instance_name,
    co_simulation_fmu_string_t  *instantiation_token,
    bool visible,
    bool logging_on,
    bool event_mode_used,
    bool early_return_allowed,
    co_simulation_fmu_list_value_reference_t *required_intermediate_variables,
    exports_fmi_fmi3_co_simulation_own_co_simulation_instance_t *ret)
{
    (void)instance_name;
    (void)instantiation_token;
    (void)visible;
    (void)logging_on;
    (void)event_mode_used;
    (void)early_return_allowed;
    (void)required_intermediate_variables;

    adder_instance_t *inst = (adder_instance_t *)calloc(1, sizeof(adder_instance_t));
    if (!inst) return false;

    inst->offset = read_resource_offset();

    *ret = exports_fmi_fmi3_co_simulation_co_simulation_instance_new(inst);
    return true;
}

/* Called by the runtime when the resource handle is dropped. */
void exports_fmi_fmi3_co_simulation_co_simulation_instance_destructor(
    adder_instance_t *rep)
{
    free(rep);
}

/* ── co-simulation interface — lifecycle methods ─────────────────────────── */

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_debug_logging(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    bool logging_on,
    co_simulation_fmu_list_string_t *categories)
{
    (void)self; (void)logging_on; (void)categories;
    return FMI_FMI3_TYPES_STATUS_OK;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_enter_initialization_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    double *maybe_tolerance,
    double start_time,
    double *maybe_stop_time)
{
    (void)maybe_tolerance; (void)start_time; (void)maybe_stop_time;
    self->in_initialization_mode = true;
    return FMI_FMI3_TYPES_STATUS_OK;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_exit_initialization_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    self->in_initialization_mode = false;
    return FMI_FMI3_TYPES_STATUS_OK;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_enter_event_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    (void)self;
    return FMI_FMI3_TYPES_STATUS_ERROR;
}

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_update_discrete_states(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    exports_fmi_fmi3_co_simulation_discrete_states_info_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)self; (void)ret;
    *err = FMI_FMI3_TYPES_STATUS_ERROR;
    return false;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_terminate(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    (void)self;
    return FMI_FMI3_TYPES_STATUS_OK;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_reset(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    /* offset is preserved across reset */
    double saved_offset          = self->offset;
    self->in_initialization_mode = false;
    self->current_time           = 0.0;
    self->input_a                = 0.0;
    self->input_b                = 0.0;
    self->offset                 = saved_offset;
    self->output_sum             = saved_offset;
    return FMI_FMI3_TYPES_STATUS_OK;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_enter_configuration_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    (void)self;
    return FMI_FMI3_TYPES_STATUS_ERROR;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_exit_configuration_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{
    (void)self;
    return FMI_FMI3_TYPES_STATUS_ERROR;
}

/* ── co-simulation interface — Float64 getter / setter ───────────────────── */

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_float64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *value_references,
    co_simulation_fmu_list_f64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    size_t n = value_references->len;
    double *buf = (double *)malloc(n * sizeof(double));
    if (!buf) { *err = FMI_FMI3_TYPES_STATUS_ERROR; return false; }

    if (!adder_impl_get_float64(self, value_references->ptr, n, buf)) {
        free(buf);
        *err = FMI_FMI3_TYPES_STATUS_ERROR;
        return false;
    }
    ret->ptr = buf;
    ret->len = n;
    return true;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_float64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *value_references,
    co_simulation_fmu_list_f64_t *values)
{
    if (value_references->len != values->len) return FMI_FMI3_TYPES_STATUS_ERROR;
    if (!adder_impl_set_float64(self, value_references->ptr, value_references->len, values->ptr))
        return FMI_FMI3_TYPES_STATUS_ERROR;
    return FMI_FMI3_TYPES_STATUS_OK;
}

/* ── All other typed getters — not supported ─────────────────────────────── */

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_float32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_f32_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_int8(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s8_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_int16(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s16_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_int32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s32_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_int64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_uint8(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u8_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_uint16(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u16_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_uint32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u32_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_uint64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_boolean(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_bool_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_string(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_string_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_binary(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_list_u8_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_clock(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_bool_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

/* ── All other typed setters — not supported ─────────────────────────────── */

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_float32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_f32_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_int8(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s8_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_int16(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s16_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_int32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s32_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_int64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_s64_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_uint8(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u8_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_uint16(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u16_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_uint32(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u32_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_uint64(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_u64_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_boolean(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_bool_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_string(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_string_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_binary(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_list_u8_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_clock(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs, co_simulation_fmu_list_bool_t *values)
{ (void)self;(void)vrs;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

/* ── co-simulation interface — variable dependencies ─────────────────────── */

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_number_of_variable_dependencies(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    exports_fmi_fmi3_co_simulation_value_reference_t vr,
    uint64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)self;
    if (vr == VR_OUTPUT_SUM) { *ret = 2; return true; }
    *err = FMI_FMI3_TYPES_STATUS_ERROR;
    return false;
}

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_variable_dependencies(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    exports_fmi_fmi3_co_simulation_value_reference_t dependent,
    exports_fmi_fmi3_co_simulation_list_variable_dependency_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)self;
    if (dependent != VR_OUTPUT_SUM) {
        *err = FMI_FMI3_TYPES_STATUS_ERROR;
        return false;
    }

    exports_fmi_fmi3_co_simulation_variable_dependency_t *deps =
        (exports_fmi_fmi3_co_simulation_variable_dependency_t *)
        malloc(2 * sizeof(exports_fmi_fmi3_co_simulation_variable_dependency_t));
    if (!deps) { *err = FMI_FMI3_TYPES_STATUS_ERROR; return false; }

    deps[0].dependent_value_reference    = VR_OUTPUT_SUM;
    deps[0].element_index_of_dependent   = 0;
    deps[0].independent_value_reference  = VR_INPUT_A;
    deps[0].element_index_of_independent = 0;
    deps[0].kind                         = FMI_FMI3_TYPES_DEPENDENCY_KIND_DEPENDENT;

    deps[1].dependent_value_reference    = VR_OUTPUT_SUM;
    deps[1].element_index_of_dependent   = 0;
    deps[1].independent_value_reference  = VR_INPUT_B;
    deps[1].element_index_of_independent = 0;
    deps[1].kind                         = FMI_FMI3_TYPES_DEPENDENCY_KIND_DEPENDENT;

    ret->ptr = deps;
    ret->len = 2;
    return true;
}

/* ── co-simulation interface — FMU state serialisation ───────────────────── */

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_fmu_state(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    exports_fmi_fmi3_co_simulation_fmu_state_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    uint8_t *buf = (uint8_t *)malloc(ADDER_STATE_SIZE);
    if (!buf) { *err = FMI_FMI3_TYPES_STATUS_ERROR; return false; }
    adder_impl_serialize_state(self, buf);
    ret->ptr = buf;
    ret->len = ADDER_STATE_SIZE;
    return true;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_fmu_state(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    exports_fmi_fmi3_co_simulation_fmu_state_t *state)
{
    if (state->len < ADDER_STATE_SIZE) return FMI_FMI3_TYPES_STATUS_ERROR;
    adder_impl_deserialize_state(self, state->ptr);
    return FMI_FMI3_TYPES_STATUS_OK;
}

/* ── co-simulation interface — directional / adjoint derivatives ─────────── */

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_directional_derivative(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *unknowns,
    co_simulation_fmu_list_value_reference_t *knowns,
    co_simulation_fmu_list_f64_t *seed,
    co_simulation_fmu_list_f64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)self;
    double *buf = (double *)malloc(sizeof(double));
    if (!buf) { *err = FMI_FMI3_TYPES_STATUS_ERROR; return false; }
    if (!adder_impl_get_dir_deriv(
            unknowns->ptr, unknowns->len,
            knowns->ptr,   knowns->len,
            seed->ptr,     seed->len,
            buf)) {
        free(buf);
        *err = FMI_FMI3_TYPES_STATUS_ERROR;
        return false;
    }
    ret->ptr = buf;
    ret->len = 1;
    return true;
}

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_adjoint_derivative(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *unknowns,
    co_simulation_fmu_list_value_reference_t *knowns,
    co_simulation_fmu_list_f64_t *seed,
    co_simulation_fmu_list_f64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)self;
    size_t n = knowns->len;
    double *buf = (double *)malloc(n * sizeof(double));
    if (!buf) { *err = FMI_FMI3_TYPES_STATUS_ERROR; return false; }
    if (!adder_impl_get_adj_deriv(
            unknowns->ptr, unknowns->len,
            knowns->ptr,   n,
            seed->ptr,     seed->len,
            buf)) {
        free(buf);
        *err = FMI_FMI3_TYPES_STATUS_ERROR;
        return false;
    }
    ret->ptr = buf;
    ret->len = n;
    return true;
}

/* ── co-simulation interface — clock interval / shift (not supported) ─────── */

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_interval_decimal(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    exports_fmi_fmi3_co_simulation_list_tuple2_f64_interval_qualifier_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_interval_fraction(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    exports_fmi_fmi3_co_simulation_list_tuple2_interval_fraction_interval_qualifier_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_shift_decimal(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    co_simulation_fmu_list_f64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_shift_fraction(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    exports_fmi_fmi3_co_simulation_list_interval_fraction_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)vrs;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_interval_decimal(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    co_simulation_fmu_list_f64_t *intervals)
{ (void)self;(void)vrs;(void)intervals; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_interval_fraction(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    exports_fmi_fmi3_co_simulation_list_interval_fraction_t *intervals)
{ (void)self;(void)vrs;(void)intervals; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_shift_decimal(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    co_simulation_fmu_list_f64_t *shifts)
{ (void)self;(void)vrs;(void)shifts; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_shift_fraction(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_value_reference_t *vrs,
    exports_fmi_fmi3_co_simulation_list_interval_fraction_t *shifts)
{ (void)self;(void)vrs;(void)shifts; return FMI_FMI3_TYPES_STATUS_ERROR; }

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_evaluate_discrete_states(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{ (void)self; return FMI_FMI3_TYPES_STATUS_ERROR; }

/* ── co-simulation interface — Co-Simulation-specific methods ────────────── */

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_set_input_derivatives(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_tuple2_value_reference_u32_t *requests,
    co_simulation_fmu_list_f64_t *values)
{ (void)self;(void)requests;(void)values; return FMI_FMI3_TYPES_STATUS_ERROR; }

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_get_output_derivatives(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    co_simulation_fmu_list_tuple2_value_reference_u32_t *requests,
    co_simulation_fmu_list_f64_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{ (void)self;(void)requests;(void)ret; *err=FMI_FMI3_TYPES_STATUS_ERROR; return false; }

bool
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_do_step(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self,
    double current_communication_point,
    double communication_step_size,
    bool   no_set_fmu_state_prior_to_current_point,
    exports_fmi_fmi3_co_simulation_do_step_result_t *ret,
    exports_fmi_fmi3_co_simulation_status_t *err)
{
    (void)no_set_fmu_state_prior_to_current_point;
    (void)err;
    adder_impl_do_step(self, current_communication_point, communication_step_size);
    ret->last_successful_time  = self->current_time;
    ret->event_handling_needed = false;
    ret->terminate_simulation  = false;
    ret->early_return          = false;
    return true;
}

exports_fmi_fmi3_co_simulation_status_t
exports_fmi_fmi3_co_simulation_method_co_simulation_instance_enter_step_mode(
    exports_fmi_fmi3_co_simulation_borrow_co_simulation_instance_t self)
{ (void)self; return FMI_FMI3_TYPES_STATUS_ERROR; }

/* ══════════════════════════════════════════════════════════════════════════
 * API glue: native FMI 3.0 C API path
 * ══════════════════════════════════════════════════════════════════════════ */

#else /* !__wasm__ */

/* ── Common ──────────────────────────────────────────────────────────────── */

FMI3_Export const char* fmi3GetVersion(void)
{
    return VERSION_STRING;
}

FMI3_Export fmi3Status fmi3SetDebugLogging(
    fmi3Instance instance,
    fmi3Boolean  loggingOn,
    size_t       nCategories,
    const fmi3String categories[])
{
    (void)instance; (void)loggingOn; (void)nCategories; (void)categories;
    return fmi3OK;
}

/* ── Instance creation / destruction ─────────────────────────────────────── */

/*
 * Read offset.txt from the FMU resource directory.
 * `resource_path` is the path to the resources/ directory as given by the
 * `resourcePath` argument of `fmi3InstantiateCoSimulation`.
 * Returns 0.0 on any error (missing path, file not found, parse failure).
 */
static double read_resource_offset_native(const char *resource_path)
{
    if (!resource_path || resource_path[0] == '\0') return 0.0;

    /* Construct "<resource_path>/offset.txt" (handle trailing separator). */
    size_t rlen = strlen(resource_path);
    size_t flen = rlen + 1 + strlen("offset.txt") + 1;
    char *path = (char *)malloc(flen);
    if (!path) return 0.0;

    int needs_sep = (resource_path[rlen - 1] != '/' &&
                     resource_path[rlen - 1] != '\\');
    if (needs_sep)
        snprintf(path, flen, "%s/offset.txt", resource_path);
    else
        snprintf(path, flen, "%soffset.txt", resource_path);

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return 0.0;

    double offset = 0.0;
    fscanf(f, "%lf", &offset);
    fclose(f);
    return offset;
}

FMI3_Export fmi3Instance fmi3InstantiateCoSimulation(
    fmi3String                     instanceName,
    fmi3String                     instantiationToken,
    fmi3String                     resourcePath,
    fmi3Boolean                    visible,
    fmi3Boolean                    loggingOn,
    fmi3Boolean                    eventModeUsed,
    fmi3Boolean                    earlyReturnAllowed,
    const fmi3ValueReference       requiredIntermediateVariables[],
    size_t                         nRequiredIntermediateVariables,
    fmi3InstanceEnvironment        instanceEnvironment,
    fmi3LogMessageCallback         logMessage,
    fmi3IntermediateUpdateCallback intermediateUpdate)
{
    (void)instanceName; (void)instantiationToken;
    (void)visible; (void)loggingOn; (void)eventModeUsed; (void)earlyReturnAllowed;
    (void)requiredIntermediateVariables; (void)nRequiredIntermediateVariables;

    adder_instance_t *inst = (adder_instance_t *)calloc(1, sizeof(adder_instance_t));
    if (!inst) return NULL;
    inst->instance_env           = instanceEnvironment;
    inst->log_cb                 = logMessage;
    inst->intermediate_update_cb = intermediateUpdate;
    inst->offset                 = read_resource_offset_native(resourcePath);
    return (fmi3Instance)inst;
}

FMI3_Export void fmi3FreeInstance(fmi3Instance instance)
{
    free(instance);
}

/* ── Lifecycle ───────────────────────────────────────────────────────────── */

FMI3_Export fmi3Status fmi3EnterInitializationMode(
    fmi3Instance instance,
    fmi3Boolean  toleranceDefined,
    fmi3Float64  tolerance,
    fmi3Float64  startTime,
    fmi3Boolean  stopTimeDefined,
    fmi3Float64  stopTime)
{
    (void)toleranceDefined; (void)tolerance; (void)startTime;
    (void)stopTimeDefined;  (void)stopTime;
    adder_instance_t *inst = (adder_instance_t *)instance;
    inst->in_initialization_mode = true;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3ExitInitializationMode(fmi3Instance instance)
{
    adder_instance_t *inst = (adder_instance_t *)instance;
    inst->in_initialization_mode = false;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3EnterEventMode(fmi3Instance instance)
{
    (void)instance;
    return fmi3Error;
}

FMI3_Export fmi3Status fmi3Terminate(fmi3Instance instance)
{
    (void)instance;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3Reset(fmi3Instance instance)
{
    adder_instance_t *inst = (adder_instance_t *)instance;
    /* offset is preserved across reset */
    inst->in_initialization_mode = false;
    inst->current_time           = 0.0;
    inst->input_a                = 0.0;
    inst->input_b                = 0.0;
    inst->output_sum             = inst->offset;
    return fmi3OK;
}

/* ── Float64 get/set ─────────────────────────────────────────────────────── */

FMI3_Export fmi3Status fmi3GetFloat64(
    fmi3Instance               instance,
    const fmi3ValueReference   valueReferences[],
    size_t                     nValueReferences,
    fmi3Float64                values[],
    size_t                     nValues)
{
    (void)nValues;
    adder_instance_t *inst = (adder_instance_t *)instance;
    return adder_impl_get_float64(inst, valueReferences, nValueReferences, values)
           ? fmi3OK : fmi3Error;
}

FMI3_Export fmi3Status fmi3SetFloat64(
    fmi3Instance               instance,
    const fmi3ValueReference   valueReferences[],
    size_t                     nValueReferences,
    const fmi3Float64          values[],
    size_t                     nValues)
{
    (void)nValues;
    adder_instance_t *inst = (adder_instance_t *)instance;
    return adder_impl_set_float64(inst, valueReferences, nValueReferences, values)
           ? fmi3OK : fmi3Error;
}

/* ── All other typed get/set stubs — not supported ───────────────────────── */

FMI3_Export fmi3Status fmi3GetFloat32(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Float32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetInt8   (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Int8 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetUInt8  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt8 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetInt16  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Int16 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetUInt16 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt16 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetInt32  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Int32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetUInt32 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetInt64  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Int64 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetUInt64 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt64 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetBoolean(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Boolean vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetString (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3String vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetBinary (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, size_t sizes[], fmi3Binary vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)sizes;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3GetClock  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Clock vals[])
{ (void)i;(void)vrs;(void)nVrs;(void)vals; return fmi3Error; }

FMI3_Export fmi3Status fmi3SetFloat32(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Float32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetInt8   (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Int8 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetUInt8  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt8 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetInt16  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Int16 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetUInt16 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt16 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetInt32  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Int32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetUInt32 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt32 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetInt64  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Int64 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetUInt64 (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt64 vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetBoolean(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Boolean vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetString (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3String vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetBinary (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const size_t sizes[], const fmi3Binary vals[], size_t nVals)
{ (void)i;(void)vrs;(void)nVrs;(void)sizes;(void)vals;(void)nVals; return fmi3Error; }
FMI3_Export fmi3Status fmi3SetClock  (fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Clock vals[])
{ (void)i;(void)vrs;(void)nVrs;(void)vals; return fmi3Error; }

/* ── Variable dependencies ───────────────────────────────────────────────── */

FMI3_Export fmi3Status fmi3GetNumberOfVariableDependencies(
    fmi3Instance      instance,
    fmi3ValueReference valueReference,
    size_t            *nDependencies)
{
    (void)instance;
    if (valueReference == VR_OUTPUT_SUM) { *nDependencies = 2; return fmi3OK; }
    return fmi3Error;
}

FMI3_Export fmi3Status fmi3GetVariableDependencies(
    fmi3Instance        instance,
    fmi3ValueReference  dependent,
    size_t              elementIndicesOfDependent[],
    fmi3ValueReference  independents[],
    size_t              elementIndicesOfIndependents[],
    fmi3DependencyKind  dependencyKinds[],
    size_t              nDependencies)
{
    (void)instance;
    if (dependent != VR_OUTPUT_SUM || nDependencies < 2) return fmi3Error;
    elementIndicesOfDependent[0]    = 0; independents[0] = VR_INPUT_A;
    elementIndicesOfIndependents[0] = 0; dependencyKinds[0] = fmi3Dependent;
    elementIndicesOfDependent[1]    = 0; independents[1] = VR_INPUT_B;
    elementIndicesOfIndependents[1] = 0; dependencyKinds[1] = fmi3Dependent;
    return fmi3OK;
}

/* ── FMU state serialisation ─────────────────────────────────────────────── */

FMI3_Export fmi3Status fmi3GetFMUState(fmi3Instance instance, fmi3FMUState *FMUState)
{
    adder_instance_t *inst = (adder_instance_t *)instance;
    uint8_t *buf = (uint8_t *)malloc(ADDER_STATE_SIZE);
    if (!buf) return fmi3Error;
    adder_impl_serialize_state(inst, buf);
    *FMUState = (fmi3FMUState)buf;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3SetFMUState(fmi3Instance instance, fmi3FMUState FMUState)
{
    adder_instance_t *inst = (adder_instance_t *)instance;
    adder_impl_deserialize_state(inst, (const uint8_t *)FMUState);
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3FreeFMUState(fmi3Instance instance, fmi3FMUState *FMUState)
{
    (void)instance;
    free(*FMUState);
    *FMUState = NULL;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3SerializedFMUStateSize(
    fmi3Instance instance, fmi3FMUState FMUState, size_t *size)
{
    (void)instance; (void)FMUState;
    *size = ADDER_STATE_SIZE;
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3SerializeFMUState(
    fmi3Instance instance, fmi3FMUState FMUState,
    fmi3Byte serializedState[], size_t size)
{
    (void)instance;
    if (size < ADDER_STATE_SIZE) return fmi3Error;
    memcpy(serializedState, FMUState, ADDER_STATE_SIZE);
    return fmi3OK;
}

FMI3_Export fmi3Status fmi3DeserializeFMUState(
    fmi3Instance      instance,
    const fmi3Byte    serializedState[],
    size_t            size,
    fmi3FMUState     *FMUState)
{
    (void)instance;
    if (size < ADDER_STATE_SIZE) return fmi3Error;
    uint8_t *buf = (uint8_t *)malloc(ADDER_STATE_SIZE);
    if (!buf) return fmi3Error;
    memcpy(buf, serializedState, ADDER_STATE_SIZE);
    *FMUState = (fmi3FMUState)buf;
    return fmi3OK;
}

/* ── Directional / adjoint derivatives ──────────────────────────────────── */

FMI3_Export fmi3Status fmi3GetDirectionalDerivative(
    fmi3Instance               instance,
    const fmi3ValueReference   unknowns[],
    size_t                     nUnknowns,
    const fmi3ValueReference   knowns[],
    size_t                     nKnowns,
    const fmi3Float64          seed[],
    size_t                     nSeed,
    fmi3Float64                sensitivity[],
    size_t                     nSensitivity)
{
    (void)instance; (void)nSensitivity;
    return adder_impl_get_dir_deriv(unknowns, nUnknowns, knowns, nKnowns,
                                    seed, nSeed, sensitivity)
           ? fmi3OK : fmi3Error;
}

FMI3_Export fmi3Status fmi3GetAdjointDerivative(
    fmi3Instance               instance,
    const fmi3ValueReference   unknowns[],
    size_t                     nUnknowns,
    const fmi3ValueReference   knowns[],
    size_t                     nKnowns,
    const fmi3Float64          seed[],
    size_t                     nSeed,
    fmi3Float64                sensitivity[],
    size_t                     nSensitivity)
{
    (void)instance; (void)nSensitivity;
    return adder_impl_get_adj_deriv(unknowns, nUnknowns, knowns, nKnowns,
                                    seed, nSeed, sensitivity)
           ? fmi3OK : fmi3Error;
}

/* ── Configuration mode (not supported) ──────────────────────────────────── */

FMI3_Export fmi3Status fmi3EnterConfigurationMode(fmi3Instance instance)
{ (void)instance; return fmi3Error; }

FMI3_Export fmi3Status fmi3ExitConfigurationMode(fmi3Instance instance)
{ (void)instance; return fmi3Error; }

/* ── Clock / interval / shift (not supported) ────────────────────────────── */

FMI3_Export fmi3Status fmi3GetIntervalDecimal(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Float64 intervals[], fmi3IntervalQualifier qualifiers[])
{ (void)i;(void)vrs;(void)nVrs;(void)intervals;(void)qualifiers; return fmi3Error; }

FMI3_Export fmi3Status fmi3GetIntervalFraction(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt64 counters[], fmi3UInt64 resolutions[], fmi3IntervalQualifier qualifiers[])
{ (void)i;(void)vrs;(void)nVrs;(void)counters;(void)resolutions;(void)qualifiers; return fmi3Error; }

FMI3_Export fmi3Status fmi3GetShiftDecimal(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3Float64 shifts[])
{ (void)i;(void)vrs;(void)nVrs;(void)shifts; return fmi3Error; }

FMI3_Export fmi3Status fmi3GetShiftFraction(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, fmi3UInt64 counters[], fmi3UInt64 resolutions[])
{ (void)i;(void)vrs;(void)nVrs;(void)counters;(void)resolutions; return fmi3Error; }

FMI3_Export fmi3Status fmi3SetIntervalDecimal(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Float64 intervals[])
{ (void)i;(void)vrs;(void)nVrs;(void)intervals; return fmi3Error; }

FMI3_Export fmi3Status fmi3SetIntervalFraction(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt64 counters[], const fmi3UInt64 resolutions[])
{ (void)i;(void)vrs;(void)nVrs;(void)counters;(void)resolutions; return fmi3Error; }

FMI3_Export fmi3Status fmi3SetShiftDecimal(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3Float64 shifts[])
{ (void)i;(void)vrs;(void)nVrs;(void)shifts; return fmi3Error; }

FMI3_Export fmi3Status fmi3SetShiftFraction(fmi3Instance i, const fmi3ValueReference vrs[], size_t nVrs, const fmi3UInt64 counters[], const fmi3UInt64 resolutions[])
{ (void)i;(void)vrs;(void)nVrs;(void)counters;(void)resolutions; return fmi3Error; }

FMI3_Export fmi3Status fmi3EvaluateDiscreteStates(fmi3Instance instance)
{ (void)instance; return fmi3Error; }

FMI3_Export fmi3Status fmi3UpdateDiscreteStates(
    fmi3Instance instance,
    fmi3Boolean *discreteStatesNeedUpdate,
    fmi3Boolean *terminateSimulation,
    fmi3Boolean *nominalsOfContinuousStatesChanged,
    fmi3Boolean *valuesOfContinuousStatesChanged,
    fmi3Boolean *nextEventTimeDefined,
    fmi3Float64 *nextEventTime)
{
    (void)instance;
    (void)discreteStatesNeedUpdate; (void)terminateSimulation;
    (void)nominalsOfContinuousStatesChanged; (void)valuesOfContinuousStatesChanged;
    (void)nextEventTimeDefined; (void)nextEventTime;
    return fmi3Error;
}

/* ── Co-Simulation-specific functions ────────────────────────────────────── */

FMI3_Export fmi3Status fmi3EnterStepMode(fmi3Instance instance)
{ (void)instance; return fmi3Error; }

FMI3_Export fmi3Status fmi3GetOutputDerivatives(
    fmi3Instance               instance,
    const fmi3ValueReference   valueReferences[],
    size_t                     nValueReferences,
    const fmi3Int32            orders[],
    fmi3Float64                values[],
    size_t                     nValues)
{ (void)instance;(void)valueReferences;(void)nValueReferences;(void)orders;(void)values;(void)nValues; return fmi3Error; }

FMI3_Export fmi3Status fmi3DoStep(
    fmi3Instance instance,
    fmi3Float64  currentCommunicationPoint,
    fmi3Float64  communicationStepSize,
    fmi3Boolean  noSetFMUStatePriorToCurrentPoint,
    fmi3Boolean *eventHandlingNeeded,
    fmi3Boolean *terminateSimulation,
    fmi3Boolean *earlyReturn,
    fmi3Float64 *lastSuccessfulTime)
{
    (void)noSetFMUStatePriorToCurrentPoint;
    adder_instance_t *inst = (adder_instance_t *)instance;
    adder_impl_do_step(inst, currentCommunicationPoint, communicationStepSize);
    *eventHandlingNeeded = fmi3False;
    *terminateSimulation = fmi3False;
    *earlyReturn         = fmi3False;
    *lastSuccessfulTime  = inst->current_time;
    return fmi3OK;
}

#endif /* !__wasm__ */
