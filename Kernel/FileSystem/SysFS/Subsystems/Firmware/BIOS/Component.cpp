/*
 * Copyright (c) 2022, Liav A. <liavalb@hotmail.co.il>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <Kernel/FileSystem/OpenFileDescription.h>
#include <Kernel/FileSystem/SysFS/Subsystems/Firmware/BIOS/Component.h>
#include <Kernel/Firmware/BIOS.h>
#include <Kernel/KBufferBuilder.h>
#include <Kernel/Memory/MemoryManager.h>
#include <Kernel/Memory/TypedMapping.h>
#include <Kernel/Sections.h>

namespace Kernel {

NonnullRefPtr<BIOSSysFSComponent> BIOSSysFSComponent::must_create(Type type, PhysicalAddress blob_paddr, size_t blob_size)
{
    return adopt_ref_if_nonnull(new (nothrow) BIOSSysFSComponent(type, blob_paddr, blob_size)).release_nonnull();
}

UNMAP_AFTER_INIT BIOSSysFSComponent::BIOSSysFSComponent(Type type, PhysicalAddress blob_paddr, size_t blob_size)
    : SysFSComponent()
    , m_blob_paddr(blob_paddr)
    , m_blob_length(blob_size)
    , m_type(type)
{
}

ErrorOr<size_t> BIOSSysFSComponent::read_bytes(off_t offset, size_t count, UserOrKernelBuffer& buffer, OpenFileDescription*) const
{
    auto blob = TRY(try_to_generate_buffer());

    if ((size_t)offset >= blob->size())
        return 0;

    ssize_t nread = min(static_cast<off_t>(blob->size() - offset), static_cast<off_t>(count));
    TRY(buffer.write(blob->data() + offset, nread));
    return nread;
}

StringView BIOSSysFSComponent::name() const
{
    switch (m_type) {
    case Type::DMIEntryPoint:
        return "smbios_entry_point"sv;
    case Type::SMBIOSTable:
        return "DMI"sv;
    default:
        break;
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<NonnullOwnPtr<KBuffer>> BIOSSysFSComponent::try_to_generate_buffer() const
{
    auto blob = TRY(Memory::map_typed<u8>((m_blob_paddr), m_blob_length));
    return KBuffer::try_create_with_bytes(Span<u8> { blob.ptr(), m_blob_length });
}
}
