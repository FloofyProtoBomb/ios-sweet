// SPDX-License-Identifier: MIT
// UNVALIDATED source overlay included in the matching NativeMetalObjects.rs.
// No fake physical device, forced family, total-RAM fallback or GPU work.
#[repr(C)]
pub struct DreMetalDeviceFacts {
    version: u32, bytes: u32, vendor: u32, chip: u32, kind: u32, flags: u32,
    max_buffer: u64, max_allocation: u64, threadgroup_memory: u64, workgroup_invocations: u64,
    axes: [u32; 3], color_attachments: u32,
}
const _: () = assert!(std::mem::size_of::<DreMetalDeviceFacts>() == 72);

/// # Safety
/// `device` is a live matching runtime handle; `out` points to a writable
/// initialized version1/72-byte descriptor owned by the caller.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_dre_device_facts(device: *const NmbDevice, out: *mut DreMetalDeviceFacts) -> bool {
    guard(false, || {
        let out = unsafe { out.as_mut() }.ok_or("null DRE device facts")?;
        if out.version != 1 || out.bytes as usize != std::mem::size_of::<DreMetalDeviceFacts>() {
            return Err("DRE device facts ABI differs".into());
        }
        let p = get(device)?.0.properties();
        if p.kind != metal2vulkan_runtime::DeviceKind::Integrated ||
            p.vendor_id != 0x5143 || p.device_id != 0x06010900 || !p.has_unified_memory {
            return Err("selected native Qualcomm A619 integrated Vulkan device required; CPU/other devices refused".into());
        }
        let l = &p.description.limits;
        let allocation = p.description.allocation.max_memory_allocation_size;
        let memory = p.max_threadgroup_memory_length.min(u64::from(l.max_compute_shared_memory_size));
        let invocations = u64::from(l.max_compute_work_group_invocations);
        let axes64 = [p.max_threads_per_threadgroup.width,p.max_threads_per_threadgroup.height,p.max_threads_per_threadgroup.depth];
        let mut axes = [0;3];
        for i in 0..3 {
            let axis = axes64[i].min(u64::from(l.max_compute_work_group_size[i])).min(invocations);
            axes[i] = u32::try_from(axis).map_err(|_| "DRE workgroup dimension does not fit ABI")?;
        }
        let colors = (MAX_COLOR_ATTACHMENTS as u32).min(l.max_color_attachments).min(l.max_fragment_output_attachments);
        if p.max_buffer_length == 0 || allocation == 0 || p.max_buffer_length > allocation ||
            memory == 0 || invocations == 0 || axes.contains(&0) || colors == 0 {
            return Err("selected Vulkan allocation/workgroup limits unavailable".into());
        }
        *out = DreMetalDeviceFacts { version:1,bytes:72,vendor:p.vendor_id,chip:p.device_id,kind:1,flags:1,
            max_buffer:p.max_buffer_length,max_allocation:allocation,threadgroup_memory:memory,
            workgroup_invocations:invocations,axes,color_attachments:colors };
        Ok(true)
    })
}
