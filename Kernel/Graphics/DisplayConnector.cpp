/*
 * Copyright (c) 2022, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Kernel/Graphics/DisplayConnector.h>
#include <Kernel/Graphics/GraphicsManagement.h>
#include <Kernel/Memory/MemoryManager.h>
#include <LibC/sys/ioctl_numbers.h>

namespace Kernel {

DisplayConnector::DisplayConnector(PhysicalAddress framebuffer_address, size_t framebuffer_resource_size, bool enable_write_combine_optimization)
    : CharacterDevice(226, GraphicsManagement::the().allocate_minor_device_number())
    , m_enable_write_combine_optimization(enable_write_combine_optimization)
    , m_framebuffer_at_arbitrary_physical_range(false)
    , m_framebuffer_address(framebuffer_address)
    , m_framebuffer_resource_size(framebuffer_resource_size)
{
}

DisplayConnector::DisplayConnector(size_t framebuffer_resource_size, bool enable_write_combine_optimization)
    : CharacterDevice(226, GraphicsManagement::the().allocate_minor_device_number())
    , m_enable_write_combine_optimization(enable_write_combine_optimization)
    , m_framebuffer_at_arbitrary_physical_range(true)
    , m_framebuffer_address({})
    , m_framebuffer_resource_size(framebuffer_resource_size)
{
}

ErrorOr<Memory::Region*> DisplayConnector::mmap(Process& process, OpenFileDescription&, Memory::VirtualRange const& range, u64 offset, int prot, bool shared)
{
    VERIFY(m_shared_framebuffer_vmobject);
    if (offset != 0)
        return Error::from_errno(ENOTSUP);

    return process.address_space().allocate_region_with_vmobject(
        range,
        *m_shared_framebuffer_vmobject,
        0,
        "Mapped Framebuffer",
        prot,
        shared);
}

ErrorOr<size_t> DisplayConnector::read(OpenFileDescription&, u64, UserOrKernelBuffer&, size_t)
{
    return Error::from_errno(ENOTIMPL);
}

ErrorOr<size_t> DisplayConnector::write(OpenFileDescription&, u64, UserOrKernelBuffer const&, size_t)
{
    return Error::from_errno(ENOTIMPL);
}

void DisplayConnector::will_be_destroyed()
{
    GraphicsManagement::the().detach_display_connector({}, *this);
    Device::will_be_destroyed();
}

void DisplayConnector::after_inserting()
{
    Device::after_inserting();
    auto rounded_size = MUST(Memory::page_round_up(m_framebuffer_resource_size));

    if (!m_framebuffer_at_arbitrary_physical_range) {
        VERIFY(m_framebuffer_address.value().page_base() == m_framebuffer_address.value());
        m_shared_framebuffer_vmobject = MUST(Memory::SharedFramebufferVMObject::try_create_for_physical_range(m_framebuffer_address.value(), rounded_size));
        m_framebuffer_region = MUST(MM.allocate_kernel_region(m_framebuffer_address.value().page_base(), rounded_size, "Framebuffer"sv, Memory::Region::Access::ReadWrite));
    } else {
        m_shared_framebuffer_vmobject = MUST(Memory::SharedFramebufferVMObject::try_create_at_arbitrary_physical_range(rounded_size));
        m_framebuffer_region = MUST(MM.allocate_kernel_region_with_vmobject(m_shared_framebuffer_vmobject->real_writes_framebuffer_vmobject(), rounded_size, "Framebuffer"sv, Memory::Region::Access::ReadWrite));
    }

    m_framebuffer_data = m_framebuffer_region->vaddr().as_ptr();
    m_fake_writes_framebuffer_region = MUST(MM.allocate_kernel_region_with_vmobject(m_shared_framebuffer_vmobject->fake_writes_framebuffer_vmobject(), rounded_size, "Fake Writes Framebuffer"sv, Memory::Region::Access::ReadWrite));

    GraphicsManagement::the().attach_new_display_connector({}, *this);
    if (m_enable_write_combine_optimization) {
        [[maybe_unused]] auto result = m_framebuffer_region->set_write_combine(true);
    }
}

bool DisplayConnector::console_mode() const
{
    VERIFY(m_control_lock.is_locked());
    return m_console_mode;
}

void DisplayConnector::set_display_mode(Badge<GraphicsManagement>, DisplayMode mode)
{
    SpinlockLocker locker(m_control_lock);

    {
        SpinlockLocker locker(m_modeset_lock);
        [[maybe_unused]] auto result = set_y_offset(0);
    }

    m_console_mode = mode == DisplayMode::Console ? true : false;
    if (m_console_mode) {
        VERIFY(m_framebuffer_region->size() == m_fake_writes_framebuffer_region->size());
        memcpy(m_fake_writes_framebuffer_region->vaddr().as_ptr(), m_framebuffer_region->vaddr().as_ptr(), m_framebuffer_region->size());
        m_shared_framebuffer_vmobject->switch_to_fake_sink_framebuffer_writes({});
        enable_console();
    } else {
        disable_console();
        m_shared_framebuffer_vmobject->switch_to_real_framebuffer_writes({});
        VERIFY(m_framebuffer_region->size() == m_fake_writes_framebuffer_region->size());
        memcpy(m_framebuffer_region->vaddr().as_ptr(), m_fake_writes_framebuffer_region->vaddr().as_ptr(), m_framebuffer_region->size());
    }
}

ErrorOr<void> DisplayConnector::initialize_edid_for_generic_monitor(Optional<Array<u8, 3>> possible_manufacturer_id_string)
{
    u8 raw_manufacturer_id[2] = { 0x0, 0x0 };
    if (possible_manufacturer_id_string.has_value()) {
        Array<u8, 3> manufacturer_id_string = possible_manufacturer_id_string.release_value();
        u8 byte1 = (((static_cast<u8>(manufacturer_id_string[0]) - '@') & 0x1f) << 2) | (((static_cast<u8>(manufacturer_id_string[1]) - '@') >> 3) & 3);
        u8 byte2 = ((static_cast<u8>(manufacturer_id_string[2]) - '@') & 0x1f) | (((static_cast<u8>(manufacturer_id_string[1]) - '@') << 5) & 0xe0);
        Array<u8, 2> manufacturer_id_string_packed_bytes = { byte1, byte2 };
        raw_manufacturer_id[0] = manufacturer_id_string_packed_bytes[1];
        raw_manufacturer_id[1] = manufacturer_id_string_packed_bytes[0];
    }

    Array<u8, 128> virtual_monitor_edid = {
        0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, /* header */
        raw_manufacturer_id[1], raw_manufacturer_id[0], /* manufacturer */
        0x00, 0x00,                                     /* product code */
        0x00, 0x00, 0x00, 0x00,                         /* serial number goes here */
        0x01,                                           /* week of manufacture */
        0x00,                                           /* year of manufacture */
        0x01, 0x03,                                     /* EDID version */
        0x80,                                           /* capabilities - digital */
        0x00,                                           /* horiz. res in cm, zero for projectors */
        0x00,                                           /* vert. res in cm */
        0x78,                                           /* display gamma (120 == 2.2). */
        0xEE,                                           /* features (standby, suspend, off, RGB, std */
                                                        /* colour space, preferred timing mode) */
        0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54,
        /* chromaticity for standard colour space. */
        0x00, 0x00, 0x00, /* no default timings */
        0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
        0x01, 0x01,
        0x01, 0x01, 0x01, 0x01, /* no standard timings */
        0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x06, 0x00, 0x02, 0x02,
        0x02, 0x02,
        /* descriptor block 1 goes below */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        /* descriptor block 2, monitor ranges */
        0x00, 0x00, 0x00, 0xFD, 0x00,
        0x00, 0xC8, 0x00, 0xC8, 0x64, 0x00, 0x0A, 0x20, 0x20, 0x20,
        0x20, 0x20,
        /* 0-200Hz vertical, 0-200KHz horizontal, 1000MHz pixel clock */
        0x20,
        /* descriptor block 3, monitor name */
        0x00, 0x00, 0x00, 0xFC, 0x00,
        'G', 'e', 'n', 'e', 'r', 'i', 'c', 'S', 'c', 'r', 'e', 'e', 'n',
        /* descriptor block 4: dummy data */
        0x00, 0x00, 0x00, 0x10, 0x00,
        0x0A, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20,
        0x00, /* number of extensions */
        0x00  /* checksum goes here */
    };
    // Note: Fix checksum to avoid warnings about checksum mismatch.
    size_t checksum = 0;
    // Note: Read all 127 bytes to add them to the checksum. Byte 128 is zeroed so
    // we could technically add it to the sum result, but it could lead to an error if it contained
    // a non-zero value, so we are not using it.
    for (size_t index = 0; index < sizeof(virtual_monitor_edid) - 1; index++)
        checksum += virtual_monitor_edid[index];
    virtual_monitor_edid[127] = 0x100 - checksum;
    set_edid_bytes(virtual_monitor_edid);
    return {};
}

void DisplayConnector::set_edid_bytes(Array<u8, 128> const& edid_bytes)
{
    memcpy((u8*)m_edid_bytes, edid_bytes.data(), sizeof(m_edid_bytes));
    if (auto parsed_edid = EDID::Parser::from_bytes({ m_edid_bytes, sizeof(m_edid_bytes) }); !parsed_edid.is_error()) {
        m_edid_parser = parsed_edid.release_value();
        m_edid_valid = true;
    } else {
        dmesgln("DisplayConnector: Print offending EDID");
        for (size_t x = 0; x < 128; x = x + 16) {
            dmesgln("{:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x} {:02x}",
                m_edid_bytes[x], m_edid_bytes[x + 1], m_edid_bytes[x + 2], m_edid_bytes[x + 3],
                m_edid_bytes[x + 4], m_edid_bytes[x + 5], m_edid_bytes[x + 6], m_edid_bytes[x + 7],
                m_edid_bytes[x + 8], m_edid_bytes[x + 9], m_edid_bytes[x + 10], m_edid_bytes[x + 11],
                m_edid_bytes[x + 12], m_edid_bytes[x + 13], m_edid_bytes[x + 14], m_edid_bytes[x + 15]);
        }
        dmesgln("DisplayConnector: Parsing EDID failed: {}", parsed_edid.error());
    }
}

ErrorOr<void> DisplayConnector::flush_rectangle(size_t, FBRect const&)
{
    return Error::from_errno(ENOTSUP);
}

DisplayConnector::ModeSetting DisplayConnector::current_mode_setting() const
{
    SpinlockLocker locker(m_modeset_lock);
    return m_current_mode_setting;
}

ErrorOr<ByteBuffer> DisplayConnector::get_edid() const
{
    if (!m_edid_valid)
        return Error::from_errno(ENOTIMPL);
    return ByteBuffer::copy(m_edid_bytes, sizeof(m_edid_bytes));
}

ErrorOr<void> DisplayConnector::ioctl(OpenFileDescription&, unsigned request, Userspace<void*> arg)
{
    if (request != GRAPHICS_IOCTL_GET_HEAD_EDID) {
        // Allow anyone to query the EDID. Eventually we'll publish the current EDID on /sys
        // so it doesn't really make sense to require the video pledge to query it.
        TRY(Process::current().require_promise(Pledge::video));
    }

    // TODO: We really should have ioctls for destroying resources as well
    switch (request) {
    case GRAPHICS_IOCTL_GET_PROPERTIES: {
        auto user_properties = static_ptr_cast<GraphicsConnectorProperties*>(arg);
        GraphicsConnectorProperties properties {};
        properties.flushing_support = flush_support();
        properties.doublebuffer_support = double_framebuffering_capable();
        properties.partial_flushing_support = partial_flush_support();
        properties.refresh_rate_support = refresh_rate_support();

        return copy_to_user(user_properties, &properties);
    }
    case GRAPHICS_IOCTL_GET_HEAD_MODE_SETTING: {
        auto user_head_mode_setting = static_ptr_cast<GraphicsHeadModeSetting*>(arg);
        GraphicsHeadModeSetting head_mode_setting {};
        TRY(copy_from_user(&head_mode_setting, user_head_mode_setting));
        {
            SpinlockLocker control_locker(m_control_lock);
            head_mode_setting.horizontal_stride = m_current_mode_setting.horizontal_stride;
            head_mode_setting.pixel_clock_in_khz = m_current_mode_setting.pixel_clock_in_khz;
            head_mode_setting.horizontal_active = m_current_mode_setting.horizontal_active;
            head_mode_setting.horizontal_front_porch_pixels = m_current_mode_setting.horizontal_front_porch_pixels;
            head_mode_setting.horizontal_sync_time_pixels = m_current_mode_setting.horizontal_sync_time_pixels;
            head_mode_setting.horizontal_blank_pixels = m_current_mode_setting.horizontal_blank_pixels;
            head_mode_setting.vertical_active = m_current_mode_setting.vertical_active;
            head_mode_setting.vertical_front_porch_lines = m_current_mode_setting.vertical_front_porch_lines;
            head_mode_setting.vertical_sync_time_lines = m_current_mode_setting.vertical_sync_time_lines;
            head_mode_setting.vertical_blank_lines = m_current_mode_setting.vertical_blank_lines;
            head_mode_setting.horizontal_offset = m_current_mode_setting.horizontal_offset;
            head_mode_setting.vertical_offset = m_current_mode_setting.vertical_offset;
        }
        return copy_to_user(user_head_mode_setting, &head_mode_setting);
    }
    case GRAPHICS_IOCTL_GET_HEAD_EDID: {
        auto user_head_edid = static_ptr_cast<GraphicsHeadEDID*>(arg);
        GraphicsHeadEDID head_edid {};
        TRY(copy_from_user(&head_edid, user_head_edid));

        auto edid_bytes = TRY(get_edid());
        if (head_edid.bytes != nullptr) {
            // Only return the EDID if a buffer was provided. Either way,
            // we'll write back the bytes_size with the actual size
            if (head_edid.bytes_size < edid_bytes.size()) {
                head_edid.bytes_size = edid_bytes.size();
                TRY(copy_to_user(user_head_edid, &head_edid));
                return Error::from_errno(EOVERFLOW);
            }
            TRY(copy_to_user(head_edid.bytes, (void const*)edid_bytes.data(), edid_bytes.size()));
        }
        head_edid.bytes_size = edid_bytes.size();
        return copy_to_user(user_head_edid, &head_edid);
    }
    case GRAPHICS_IOCTL_SET_HEAD_MODE_SETTING: {
        auto user_mode_setting = static_ptr_cast<GraphicsHeadModeSetting const*>(arg);
        auto head_mode_setting = TRY(copy_typed_from_user(user_mode_setting));

        if (head_mode_setting.horizontal_stride < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.pixel_clock_in_khz < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.horizontal_active < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.horizontal_front_porch_pixels < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.horizontal_sync_time_pixels < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.horizontal_blank_pixels < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.vertical_active < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.vertical_front_porch_lines < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.vertical_sync_time_lines < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.vertical_blank_lines < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.horizontal_offset < 0)
            return Error::from_errno(EINVAL);
        if (head_mode_setting.vertical_offset < 0)
            return Error::from_errno(EINVAL);
        {
            SpinlockLocker control_locker(m_control_lock);
            ModeSetting requested_mode_setting;
            requested_mode_setting.horizontal_stride = 0;
            requested_mode_setting.pixel_clock_in_khz = head_mode_setting.pixel_clock_in_khz;
            requested_mode_setting.horizontal_active = head_mode_setting.horizontal_active;
            requested_mode_setting.horizontal_front_porch_pixels = head_mode_setting.horizontal_front_porch_pixels;
            requested_mode_setting.horizontal_sync_time_pixels = head_mode_setting.horizontal_sync_time_pixels;
            requested_mode_setting.horizontal_blank_pixels = head_mode_setting.horizontal_blank_pixels;
            requested_mode_setting.vertical_active = head_mode_setting.vertical_active;
            requested_mode_setting.vertical_front_porch_lines = head_mode_setting.vertical_front_porch_lines;
            requested_mode_setting.vertical_sync_time_lines = head_mode_setting.vertical_sync_time_lines;
            requested_mode_setting.vertical_blank_lines = head_mode_setting.vertical_blank_lines;
            requested_mode_setting.horizontal_offset = head_mode_setting.horizontal_offset;
            requested_mode_setting.vertical_offset = head_mode_setting.vertical_offset;

            TRY(set_mode_setting(requested_mode_setting));
        }
        return {};
    }

    case GRAPHICS_IOCTL_SET_SAFE_HEAD_MODE_SETTING: {
        SpinlockLocker control_locker(m_control_lock);
        TRY(set_safe_mode_setting());
        return {};
    }

    case GRAPHICS_IOCTL_SET_HEAD_VERTICAL_OFFSET_BUFFER: {
        // FIXME: We silently ignore the request if we are in console mode.
        // WindowServer is not ready yet to handle errors such as EBUSY currently.
        SpinlockLocker control_locker(m_control_lock);
        if (console_mode()) {
            return {};
        }

        auto user_head_vertical_buffer_offset = static_ptr_cast<GraphicsHeadVerticalOffset const*>(arg);
        auto head_vertical_buffer_offset = TRY(copy_typed_from_user(user_head_vertical_buffer_offset));

        SpinlockLocker locker(m_modeset_lock);

        if (head_vertical_buffer_offset.offsetted < 0 || head_vertical_buffer_offset.offsetted > 1)
            return Error::from_errno(EINVAL);
        TRY(set_y_offset(head_vertical_buffer_offset.offsetted == 0 ? 0 : m_current_mode_setting.vertical_active));
        if (head_vertical_buffer_offset.offsetted == 0)
            m_vertical_offsetted = false;
        else
            m_vertical_offsetted = true;
        return {};
    }
    case GRAPHICS_IOCTL_GET_HEAD_VERTICAL_OFFSET_BUFFER: {
        auto user_head_vertical_buffer_offset = static_ptr_cast<GraphicsHeadVerticalOffset*>(arg);
        GraphicsHeadVerticalOffset head_vertical_buffer_offset {};
        TRY(copy_from_user(&head_vertical_buffer_offset, user_head_vertical_buffer_offset));

        head_vertical_buffer_offset.offsetted = m_vertical_offsetted;
        return copy_to_user(user_head_vertical_buffer_offset, &head_vertical_buffer_offset);
    }
    case GRAPHICS_IOCTL_FLUSH_HEAD_BUFFERS: {
        if (console_mode())
            return {};
        if (!partial_flush_support())
            return Error::from_errno(ENOTSUP);
        MutexLocker locker(m_flushing_lock);
        auto user_flush_rects = static_ptr_cast<FBFlushRects const*>(arg);
        auto flush_rects = TRY(copy_typed_from_user(user_flush_rects));
        if (Checked<unsigned>::multiplication_would_overflow(flush_rects.count, sizeof(FBRect)))
            return Error::from_errno(EFAULT);
        if (flush_rects.count > 0) {
            for (unsigned i = 0; i < flush_rects.count; i++) {
                FBRect user_dirty_rect;
                TRY(copy_from_user(&user_dirty_rect, &flush_rects.rects[i]));
                {
                    SpinlockLocker control_locker(m_control_lock);
                    if (console_mode()) {
                        return {};
                    }
                    TRY(flush_rectangle(flush_rects.buffer_index, user_dirty_rect));
                }
            }
        }
        return {};
    };
    case GRAPHICS_IOCTL_FLUSH_HEAD: {
        // FIXME: We silently ignore the request if we are in console mode.
        // WindowServer is not ready yet to handle errors such as EBUSY currently.
        MutexLocker locker(m_flushing_lock);
        SpinlockLocker control_locker(m_control_lock);
        if (console_mode()) {
            return {};
        }

        if (!flush_support())
            return Error::from_errno(ENOTSUP);

        TRY(flush_first_surface());
        return {};
    }
    }
    return EINVAL;
}

}
