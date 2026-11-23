// SPDX-FileCopyrightText: 2026 syunnPC
// SPDX-License-Identifier: Apache-2.0

#include "Media.h"
#include "Localization.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>

namespace Ia64
{
namespace
{
constexpr uint32_t SectorSize = 2048;
constexpr uint32_t MetadataLimit = 2 * 1024 * 1024;

uint32_t Little(std::string_view bytes, size_t offset, size_t count = 4)
{
    uint32_t value = 0;
    for (size_t index = 0; index < count; ++index)
    {
        value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + index])) << (8 * index);
    }
    return value;
}

class Image
{
public:
    explicit Image(const Fs::path& path)
        : _file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        if (!_file)
        {
            WinError(LText(IDS_IsoOpen));
        }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(_file.Get(), &size))
        {
            WinError(LText(IDS_IsoSize));
        }
        _size = static_cast<uint64_t>(size.QuadPart);
    }

    uint64_t Sectors() const
    {
        return _size / SectorSize;
    }

    std::string Read(uint64_t sector, uint32_t count = SectorSize) const
    {
        if (sector >= Sectors())
        {
            return {};
        }
        const auto offset = sector * SectorSize;
        count = static_cast<uint32_t>(std::min<uint64_t>(count, _size - offset));
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(_file.Get(), position, nullptr, FILE_BEGIN))
        {
            WinError(LText(IDS_IsoSeek));
        }
        std::string buffer(count, '\0');
        DWORD size{};
        if (!ReadFile(_file.Get(), buffer.data(), count, &size, nullptr))
        {
            WinError(LText(IDS_IsoRead));
        }
        buffer.resize(size);
        return buffer;
    }

private:
    Handle _file;
    uint64_t _size{};
};

std::string Lower(std::string text)
{
    for (auto& character : text)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character + ('a' - 'A'));
        }
    }
    return text;
}

std::string_view Strip(std::string_view value)
{
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos)
    {
        return {};
    }
    return value.substr(start, value.find_last_not_of(" \t\r\n") - start + 1);
}

bool Contains(std::string_view text, std::initializer_list<std::string_view> patterns)
{
    return std::ranges::any_of(patterns, [text](auto item) { return text.contains(item); });
}

struct Extent
{
    uint32_t Sector{};
    uint32_t Length{};
};

struct VolumeDescriptors
{
    std::optional<Extent> IsoRoot;
    std::string Label;
    bool HasUdf{};
    bool Optical() const
    {
        return IsoRoot.has_value() || HasUdf;
    }
};

VolumeDescriptors ReadVolumeDescriptors(const Image& image)
{
    VolumeDescriptors volume;
    const auto bytes = image.Read(0, 64 * SectorSize);
    // A QCOW container can have optical volume descriptors in its guest data.
    if (bytes.starts_with("QFI\xfb"))
    {
        return volume;
    }
    for (size_t offset = 16 * SectorSize; offset + SectorSize <= bytes.size(); offset += SectorSize)
    {
        const auto sector = std::string_view(bytes).substr(offset, SectorSize);
        const auto id = sector.substr(1, 5);
        if (id == "NSR02" || id == "NSR03")
        {
            volume.HasUdf = true;
        }
        if (!volume.IsoRoot && id == "CD001" && sector[0] == 1 && Little(sector, 128, 2) == SectorSize)
        {
            volume.IsoRoot = Extent{Little(sector, 158), Little(sector, 166)};
            volume.Label = Lower(std::string(Strip(sector.substr(40, 32))));
        }
    }
    return volume;
}

bool IndexedDirectory(std::string_view name)
{
    constexpr std::array<std::string_view, 4> directories{"ia64", "i386", "amd64", "sources"};
    return std::ranges::find(directories, name) != directories.end();
}

class IsoIndex
{
public:
    IsoIndex(const Image& image, const std::optional<Extent>& root) : _image(image)
    {
        if (root)
        {
            Index(*root, "", 0);
        }
    }

    bool Has(std::string_view path) const
    {
        return _files.contains(path);
    }

    std::string File(std::string_view path) const
    {
        const auto found = _files.find(path);
        if (found == _files.end())
        {
            return {};
        }
        return _image.Read(found->second.Sector, std::min(found->second.Length, MetadataLimit));
    }

private:
    void Index(Extent entry, const std::string& prefix, int depth)
    {
        if (entry.Length > MetadataLimit || depth > 3 || _files.size() >= 12000)
        {
            return;
        }
        const auto directory = _image.Read(entry.Sector, entry.Length);
        for (size_t offset = 0; offset < directory.size() && _files.size() < 12000;)
        {
            const auto length = static_cast<unsigned char>(directory[offset]);
            if (!length)
            {
                offset = (offset / SectorSize + 1) * SectorSize;
                continue;
            }
            if (length < 34 || length > directory.size() - offset)
            {
                break;
            }
            const std::string_view record(directory.data() + offset, length);
            offset += length;
            const auto nameLength = static_cast<unsigned char>(record[32]);
            if (!nameLength || 33u + nameLength > record.size() ||
                (nameLength == 1 && (record[33] == '\0' || record[33] == '\1')))
            {
                continue;
            }
            auto name = Lower(std::string(record.substr(33, nameLength)));
            if (const auto version = name.find(';'); version != std::string::npos)
            {
                name.resize(version);
            }
            if (name.ends_with('.'))
            {
                name.pop_back();
            }
            const auto path = prefix + name;
            const Extent child{Little(record, 2), Little(record, 10)};
            _files.try_emplace(path, child);
            if ((record[25] & 2) && IndexedDirectory(name))
            {
                Index(child, path + "/", depth + 1);
            }
        }
    }

    const Image& _image;
    std::map<std::string, Extent, std::less<>> _files;
};

bool HasTag(std::string_view value, unsigned tag)
{
    if (value.size() < 16 || Little(value, 0, 2) != tag)
    {
        return false;
    }
    unsigned checksum = 0;
    for (size_t index = 0; index < 16; ++index)
    {
        if (index != 4)
        {
            checksum += static_cast<unsigned char>(value[index]);
        }
    }
    return (checksum & 0xff) == static_cast<unsigned char>(value[4]);
}

struct UdfAddress
{
    uint32_t Block{};
    uint32_t Partition{};
};

UdfAddress LongAddress(std::string_view value, size_t offset)
{
    return {Little(value, offset + 4), Little(value, offset + 8, 2)};
}

std::string UdfName(std::string_view identifier)
{
    if (identifier.empty() || (identifier[0] != 8 && identifier[0] != 16))
    {
        return {};
    }
    const bool wide = identifier[0] == 16;
    identifier.remove_prefix(1);
    if (wide && identifier.size() % 2)
    {
        return {};
    }
    std::string result;
    for (size_t index = 0; index < identifier.size(); index += wide ? 2 : 1)
    {
        if (wide && identifier[index] != '\0')
        {
            return {};
        }
        const auto character = static_cast<unsigned char>(identifier[index + (wide ? 1 : 0)]);
        if (character < 32 || character > 126)
        {
            return {};
        }
        result.push_back(static_cast<char>(character));
    }
    return Lower(std::move(result));
}

// Optical installation media use ECMA-167 physical partitions. Only metadata
// paths needed for detection are read, without opening or mounting the disc.
class UdfIndex
{
public:
    UdfIndex(const Image& image, bool present) : _image(image)
    {
        if (!present || image.Sectors() < 257)
        {
            return;
        }

        const std::array anchors{uint64_t{256}, image.Sectors() - 1, image.Sectors() - 257};
        for (const auto sector : anchors)
        {
            const auto anchor = _image.Read(sector);
            if (!HasTag(anchor, 2))
            {
                continue;
            }
            if (Open({Little(anchor, 20), Little(anchor, 16)}) || Open({Little(anchor, 28), Little(anchor, 24)}))
            {
                break;
            }
        }
    }

    bool Has(std::string_view path) const
    {
        return Find(path).has_value();
    }

    std::string File(std::string_view path) const
    {
        const auto address = Find(path);
        return address ? Contents(*address) : std::string{};
    }

private:
    bool Open(Extent descriptors)
    {
        if (descriptors.Length < SectorSize || descriptors.Length > MetadataLimit)
        {
            return false;
        }
        std::map<uint32_t, Extent> partitions;
        std::string logical;
        for (uint32_t index = 0; index < descriptors.Length / SectorSize; ++index)
        {
            auto block = _image.Read(uint64_t{descriptors.Sector} + index);
            if (block.size() != SectorSize || HasTag(block, 8))
            {
                break;
            }
            if (HasTag(block, 5))
            {
                partitions[Little(block, 22, 2)] = {Little(block, 188), Little(block, 192)};
            }
            if (HasTag(block, 6) && Little(block, 212) == SectorSize)
            {
                logical = std::move(block);
            }
        }
        if (logical.empty())
        {
            return false;
        }
        const auto mapLength = Little(logical, 264);
        const auto mapCount = Little(logical, 268);
        if (!mapCount || mapLength > logical.size() - 440)
        {
            return false;
        }
        _partitions.clear();
        size_t offset = 440;
        const size_t end = offset + mapLength;
        for (uint32_t index = 0; index < mapCount; ++index)
        {
            if (end - offset < 2)
            {
                return false;
            }
            const auto size = static_cast<unsigned char>(logical[offset + 1]);
            if (size < 2 || size > end - offset)
            {
                return false;
            }
            if (logical[offset] == 1 && size == 6)
            {
                const auto found = partitions.find(Little(logical, offset + 4, 2));
                if (found != partitions.end())
                {
                    _partitions.emplace(index, found->second);
                }
            }
            offset += size;
        }
        const auto fileset = Block(LongAddress(logical, 248));
        if (fileset.size() != SectorSize || !HasTag(fileset, 256))
        {
            return false;
        }
        _root = LongAddress(fileset, 400);
        return true;
    }

    std::string Block(UdfAddress address, uint32_t size = SectorSize) const
    {
        const auto partition = _partitions.find(address.Partition);
        if (partition == _partitions.end() || address.Block >= partition->second.Length)
        {
            return {};
        }
        const auto remaining = uint64_t{partition->second.Length - address.Block} * SectorSize;
        if (size > remaining)
        {
            return {};
        }
        return _image.Read(uint64_t{partition->second.Sector} + address.Block, size);
    }

    std::string Contents(UdfAddress address) const
    {
        const auto entry = Block(address);
        const bool extended = HasTag(entry, 266);
        if ((!extended && !HasTag(entry, 261)) || entry.size() != SectorSize)
        {
            return {};
        }
        const size_t header = extended ? 216 : 176;
        const size_t attributes = Little(entry, header - 8);
        const size_t allocation = Little(entry, header - 4);
        if (attributes > entry.size() - header || allocation > entry.size() - header - attributes)
        {
            return {};
        }
        const auto data = std::string_view(entry).substr(header + attributes, allocation);
        const auto fieldLength = uint64_t{Little(entry, 56)} | (uint64_t{Little(entry, 60)} << 32);
        auto wanted = static_cast<uint32_t>(std::min<uint64_t>(fieldLength, MetadataLimit));
        const auto kind = Little(entry, 34, 2) & 7;
        if (kind == 3)
        {
            return std::string(data.substr(0, wanted));
        }
        if (kind > 1)
        {
            return {};
        }
        const size_t width = kind == 0 ? 8 : 16;
        std::string result;
        for (size_t offset = 0; wanted && offset + width <= data.size(); offset += width)
        {
            const auto length = Little(data, offset);
            if (length >> 30)
            {
                return {};
            }
            const auto count = std::min(wanted, length);
            const auto extent =
                kind == 0 ? UdfAddress{Little(data, offset + 4), address.Partition} : LongAddress(data, offset);
            auto bytes = Block(extent, count);
            if (bytes.size() != count)
            {
                return {};
            }
            result += bytes;
            wanted -= count;
        }
        return result;
    }

    std::optional<UdfAddress> Find(std::string_view path) const
    {
        auto current = _root;
        while (current && !path.empty())
        {
            const auto slash = path.find('/');
            const auto name = path.substr(0, slash);
            const auto directory = Contents(*current);
            current.reset();
            for (size_t offset = 0; offset + 38 <= directory.size();)
            {
                const auto record = std::string_view(directory).substr(offset);
                if (!HasTag(record, 257))
                {
                    break;
                }
                const auto flags = static_cast<unsigned char>(record[18]);
                const auto length = static_cast<unsigned char>(record[19]);
                const size_t start = 38u + Little(record, 36, 2);
                const size_t size = start + length;
                if (size > record.size())
                {
                    break;
                }
                if (!(flags & 12u) && (slash == std::string_view::npos || (flags & 2u)) &&
                    UdfName(record.substr(start, length)) == name)
                {
                    current = LongAddress(record, 20);
                    break;
                }
                offset += (size + 3) & ~size_t{3};
            }
            if (slash == std::string_view::npos)
            {
                break;
            }
            path.remove_prefix(slash + 1);
        }
        return current;
    }

    const Image& _image;
    std::map<uint32_t, Extent> _partitions;
    std::optional<UdfAddress> _root;
};

struct WindowsVersion
{
    unsigned Major{};
    unsigned Minor{};
    unsigned Build{};
    bool Ia64{};
};

std::optional<WindowsVersion> SetupFileVersion(std::string_view bytes)
{
    if (bytes.size() < 64 || !bytes.starts_with("MZ"))
    {
        return std::nullopt;
    }
    const auto pe = Little(bytes, 60);
    if (pe > bytes.size() - 6 || bytes.substr(pe, 4) != std::string_view("PE\0\0", 4))
    {
        return std::nullopt;
    }

    constexpr std::string_view signature("\xbd\x04\xef\xfe\0\0\x01\0", 8);
    constexpr std::string_view key("V\0S\0_\0V\0E\0R\0S\0I\0O\0N\0_\0I\0N\0F\0O\0", 30);
    for (auto pos = bytes.find(signature, pe); pos != std::string_view::npos;
         pos = bytes.find(signature, pos + signature.size()))
    {
        const auto start = pos > 64 ? pos - 64 : 0;
        if (bytes.size() - pos < 52 || !bytes.substr(start, pos - start).contains(key))
        {
            continue;
        }
        const auto high = Little(bytes, pos + 8);
        const auto low = Little(bytes, pos + 12);
        if (const auto major = high >> 16; major >= 3 && major <= 20)
        {
            return WindowsVersion{major, high & 0xffff, low >> 16, Little(bytes, pe + 4, 2) == 0x0200};
        }
    }
    return std::nullopt;
}

MediaRecommendation WindowsMedia(const WindowsVersion& version, std::string_view label)
{
    if (version.Build < 3700)
    {
        return {LText(IDS_IsoWindowsLegacy), L"itanium-vpc"};
    }

    if (version.Major == 6 && version.Minor == 1 && (version.Build == 7600 || version.Build == 7601) &&
        (version.Ia64 || Contains(label, {"grmsiai", "grmsp1iai", "srv2008r2"})))
    {
        return {LText(IDS_IsoR2), L"itanium2-vpc", true};
    }

    return {LText(IDS_IsoWindows), L"itanium2-vpc"};
}
} // namespace

bool HasIsoExtension(const Fs::path& path)
{
    return _wcsicmp(path.extension().c_str(), L".iso") == 0;
}

bool IsOpticalImage(const Fs::path& path)
{
    return HasIsoExtension(path) || (IsFile(path) && ReadVolumeDescriptors(Image(path)).Optical());
}

MediaRecommendation InspectIso(const Fs::path& path)
{
    if (path.empty())
    {
        return {L"", L"itanium2-vpc"};
    }
    const Image image(path);
    const auto volume = ReadVolumeDescriptors(image);
    if (!volume.Optical())
    {
        return {LText(IDS_IsoUnknown), L"itanium2-vpc"};
    }
    const IsoIndex iso(image, volume.IsoRoot);
    const UdfIndex udf(image, volume.HasUdf);

    const auto has = [&](std::string_view file) { return iso.Has(file) || udf.Has(file); };
    const auto read = [&](std::string_view file) { return iso.Has(file) ? iso.File(file) : udf.File(file); };

    const bool windows = has("win51") || has("win51.b2") || has("win51.rc1") || has("bootmgr") || has("bootmgr.efi") ||
                         has("sources/install.wim") || has("sources/install.esd") || has("ia64/txtsetup.sif") ||
                         has("i386/txtsetup.sif") || has("amd64/txtsetup.sif") ||
                         (has("srsp2.cmd") && (has("readmesp.htm") || has("cdlaunch")));
    if (windows)
    {
        // The setup program's fixed FileVersion identifies the Windows build.
        for (const auto name :
             {"sources/setup.exe", "ia64/setup.exe", "setup.exe", "amd64/setup.exe", "i386/setup.exe"})
        {
            if (const auto version = SetupFileVersion(read(name)))
            {
                return WindowsMedia(*version, volume.Label);
            }
        }
        return {LText(IDS_IsoWindows), L"itanium2-vpc"};
    }

    if (Contains(volume.Label,
                 {"linux", "debian", "ubuntu", "fedora", "centos", "red hat", "rhel", "suse", "sles", "gentoo"}) ||
        has("isolinux") || (has("livecd") && has("image.squashfs")) || (has("dists") && has("pool")))
    {
        return {LText(IDS_IsoLinux), L"itanium2-vpc"};
    }

    return {LText(IDS_IsoOther), L"hp-rx2660"};
}
} // namespace Ia64
