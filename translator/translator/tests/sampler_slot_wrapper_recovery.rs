//! A local pointer slot that holds a sampler is read back through the
//! single-field `%"struct.metal::sampler"` wrapper AIR emits for a sampler
//! pointer: `bitcast` the slot pointer, then `getelementptr ... i64 0, i32 0`
//! to reach the member. That member sits at byte offset 0, so the recovering
//! load must resolve to the very slot the store wrote. When it did not, the CA
//! fragment `fixed_frag_lpf_cpf` (a WindowServer shader) was refused with
//! "sampler operand is a pointer whose exact sampler state was not recovered".
//!
//! Both arms are covered: the runtime arm (the sampler is an entry parameter
//! stored into the slot) and the static arm (the slot is the constant
//! `@_ZL14unused_sampler` wrapper holding `@__air_sampler_state`).

use metal2vulkan::passes::Stage;
use metal2vulkan::translate_sanitized_native;
use std::path::PathBuf;

fn tmp() -> PathBuf {
    let d = std::env::temp_dir().join(format!("m2v_sampler_wrapper_{}", std::process::id()));
    let _ = std::fs::create_dir_all(&d);
    d
}

const RUNTIME_ARM: &str = r#"target triple = "air64_v28-apple-macosx26.5.0"

%"struct.metal::sampler" = type { ptr addrspace(2) }

define <4 x float> @frag(ptr addrspace(1) %tex, ptr addrspace(2) %smp, <2 x float> %coord) {
entry:
  %slot = alloca ptr addrspace(2), align 8
  store ptr addrspace(2) %smp, ptr %slot, align 8
  %cast = bitcast ptr %slot to %"struct.metal::sampler"*
  %field = getelementptr inbounds %"struct.metal::sampler", %"struct.metal::sampler"* %cast, i64 0, i32 0
  %loaded = load ptr addrspace(2), ptr %field, align 8
  %s = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %loaded, <2 x float> %coord, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %s, 0
  ret <4 x float> %c
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!air.fragment = !{!0}
!0 = !{ptr @frag, !1, !2}
!1 = !{!9}
!9 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!2 = !{!3, !4, !6}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!4 = !{i32 1, !"air.sampler", !"air.location_index", i32 0, i32 1, !"air.arg_type_name", !"sampler", !"air.arg_name", !"smp"}
!6 = !{i32 2, !"air.fragment_input", !"generated(coord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"coord"}
"#;

const STATIC_ARM: &str = r#"target triple = "air64_v28-apple-macosx26.5.0"

%"struct.metal::sampler" = type { ptr addrspace(2) }
@__air_sampler_state = internal addrspace(2) constant i64 -9188470239253757879, align 8
@_ZL14unused_sampler = internal unnamed_addr constant %"struct.metal::sampler" { ptr addrspace(2) bitcast (i64 addrspace(2)* @__air_sampler_state to ptr addrspace(2)) }

define <4 x float> @frag(ptr addrspace(1) %tex, <2 x float> %coord) {
entry:
  %field = getelementptr inbounds %"struct.metal::sampler", %"struct.metal::sampler"* @_ZL14unused_sampler, i64 0, i32 0
  %loaded = load ptr addrspace(2), ptr %field, align 8
  %s = call { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1) %tex, ptr addrspace(2) %loaded, <2 x float> %coord, i1 true, <2 x i32> zeroinitializer, i1 false, float 0.000000e+00, float 0.000000e+00, i32 0)
  %c = extractvalue { <4 x float>, i8 } %s, 0
  ret <4 x float> %c
}

declare { <4 x float>, i8 } @air.sample_texture_2d.v4f32(ptr addrspace(1), ptr addrspace(2), <2 x float>, i1, <2 x i32>, i1, float, float, i32)

!air.fragment = !{!0}
!0 = !{ptr @frag, !1, !2}
!1 = !{!9}
!9 = !{!"air.render_target", i32 0, i32 0, !"air.arg_type_name", !"float4"}
!2 = !{!3, !6}
!3 = !{i32 0, !"air.texture", !"air.location_index", i32 0, i32 1, !"air.sample", !"air.arg_type_name", !"texture2d<float, sample>", !"air.arg_name", !"tex"}
!6 = !{i32 1, !"air.fragment_input", !"generated(coord)", !"air.center", !"air.perspective", !"air.arg_type_name", !"float2", !"air.arg_name", !"coord"}
!air.sampler_states = !{!7}
!7 = !{!"air.sampler_state", ptr addrspace(2) @__air_sampler_state}
"#;

#[test]
fn a_sampler_loaded_through_the_wrapper_reuses_the_stored_slot() {
    match translate_sanitized_native(RUNTIME_ARM, Stage::Fragment, &tmp()) {
        Ok(spv) => assert!(
            spv.len() > 0,
            "the member-0 wrapper GEP must recover the stored sampler slot"
        ),
        Err(e) => panic!(
            "the sampler slot written by the store must be recovered by the \
             wrapper GEP load instead of falling back: {e}"
        ),
    }
}

#[test]
fn a_static_wrapper_sampler_still_recovers_its_exact_state() {
    match translate_sanitized_native(STATIC_ARM, Stage::Fragment, &tmp()) {
        Ok(spv) => assert!(
            spv.len() > 0,
            "the constant wrapper global must still recover its sampler state"
        ),
        Err(e) => panic!(
            "the constant `%\"struct.metal::sampler\"` wrapper must keep \
             recovering its exact sampler state: {e}"
        ),
    }
}
