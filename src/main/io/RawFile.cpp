/*
 * VGMTrans (c) 2002-2019
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#include "RawFile.h"

#include <fstream>
#include <stdexcept>
#include "LogManager.h"
#include "components/GTBFile.h"
#include "util/BytePattern.h"

/* RawFile */

/* FIXME: we own the GTBFile, should use unique_ptr instead */
void RawFile::addContainedGTBFile(std::shared_ptr<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>> GTBfile) {
    m_GTBfiles.emplace_back(GTBfile);
}

void RawFile::removeContainedGTBFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *> GTBfile) {
    auto iter = std::ranges::find_if(m_GTBfiles, [GTBfile](auto file) { return *file == GTBfile; });
    if (iter != m_GTBfiles.end())
        m_GTBfiles.erase(iter);
    else {
        L_WARN("Requested deletion for GTBFile but it was not found");
    }
}

uint32_t RawFile::readBytes(size_t offset, uint32_t nCount, void *pBuffer) const {
    memcpy(pBuffer, data() + offset, nCount);
    return nCount;
}

bool RawFile::matchBytes(const uint8_t *pattern, size_t offset, size_t nCount) const {
    return memcmp(data() + offset, pattern, nCount) == 0;
}

bool RawFile::matchBytePattern(const BytePattern &pattern, size_t offset) const {
    return pattern.match(data() + offset, pattern.length());
}

bool RawFile::searchBytePattern(const BytePattern &pattern, uint32_t &nMatchOffset,
                                uint32_t nSearchOffset, uint32_t nSearchSize) const {
    if (nSearchOffset >= size())
        return false;

    if ((nSearchOffset + nSearchSize) > size())
        nSearchSize = size() - nSearchOffset;

    if (nSearchSize < pattern.length())
        return false;

    for (size_t offset = nSearchOffset; offset < nSearchOffset + nSearchSize - pattern.length();
         offset++) {
        if (matchBytePattern(pattern, offset)) {
            nMatchOffset = offset;
            return true;
        }
    }
    return false;
}

std::string RawFile::readNullTerminatedString(size_t offset, size_t maxLength) const {
  const char* stringPtr = data() + offset;
  size_t length = strnlen(stringPtr, maxLength);
  return std::string(stringPtr, length);
}

/* DiskFile */

DiskFile::DiskFile(const std::string &path) : m_path(path) {
    std::error_code error;
    m_data.map(path, error);
    if (error) {
        throw std::runtime_error("Failed to memory-map file: " + path + " (" + error.message() + ")");
    }
}

/* EditableRawFile */

EditableRawFile::EditableRawFile(const std::string &path) : m_path(path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        throw std::runtime_error("Unable to open file for editing: " + path);
    }
    m_data.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool EditableRawFile::writeByte(size_t offset, uint8_t value) {
    if (!isValidOffset(offset)) {
        return false;
    }
    m_data[offset] = static_cast<char>(value);
    m_dirty = true;
    return true;
}

bool EditableRawFile::writeBytes(size_t offset, const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
        return true;
    }
    if (offset >= size() || offset + bytes.size() > size()) {
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        m_data[offset + i] = static_cast<char>(bytes[i]);
    }
    m_dirty = true;
    return true;
}

bool EditableRawFile::flush() {
    if (!m_dirty) {
        return true;
    }
    std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        L_ERROR("Failed to open {} for writing", m_path.string());
        return false;
    }
    stream.write(m_data.data(), static_cast<std::streamsize>(m_data.size()));
    if (!stream) {
        L_ERROR("Failed while writing {}", m_path.string());
        return false;
    }
    m_dirty = false;
    return true;
}

/* VirtFile */

VirtFile::VirtFile(const RawFile &file, size_t offset) : m_name(file.name()), m_lpath(file.path()) {
    std::copy(file.begin() + offset, file.end(), std::back_inserter(m_data));
}

VirtFile::VirtFile(const RawFile &file, size_t offset, size_t limit)
    : m_name(file.name()), m_lpath(file.path()) {
    std::copy_n(file.data() + offset, limit, std::back_inserter(m_data));
}

VirtFile::VirtFile(const uint8_t *data, uint32_t fileSize, std::string name,
                   std::string parent_fullpath, const GTBTag& tag)
    : m_name(std::move(name)), m_lpath(std::move(parent_fullpath)) {
  std::copy_n(data, fileSize, std::back_inserter(m_data));
  this->tag = tag;
}

/* EditableSliceFile */

EditableSliceFile::EditableSliceFile(const RawFile &parent,
                                     size_t sliceOffset,
                                     size_t sliceLength,
                                     std::string nameOverride,
                                     const GTBTag &tag)
    : m_buffer(parent.begin(), parent.end()),
      m_viewOffset(sliceOffset),
      m_viewLength(sliceLength),
      m_destPath(parent.path()),
      m_name(nameOverride.empty() ? parent.name() : std::move(nameOverride)) {
    this->tag = tag;
    if (m_viewOffset + m_viewLength > m_buffer.size()) {
        throw std::runtime_error("EditableSliceFile view exceeds parent buffer size");
    }
}

uint16_t EditableSliceFile::readShort(size_t offset) const {
    const size_t base = m_viewOffset + offset;
    return static_cast<uint16_t>(static_cast<uint8_t>(m_buffer[base]) |
                                 (static_cast<uint8_t>(m_buffer[base + 1]) << 8));
}

uint32_t EditableSliceFile::readWord(size_t offset) const {
    const size_t base = m_viewOffset + offset;
    return static_cast<uint32_t>(static_cast<uint8_t>(m_buffer[base]) |
                                 (static_cast<uint8_t>(m_buffer[base + 1]) << 8) |
                                 (static_cast<uint8_t>(m_buffer[base + 2]) << 16) |
                                 (static_cast<uint8_t>(m_buffer[base + 3]) << 24));
}

uint16_t EditableSliceFile::readShortBE(size_t offset) const {
    const size_t base = m_viewOffset + offset;
    return static_cast<uint16_t>((static_cast<uint8_t>(m_buffer[base]) << 8) |
                                 static_cast<uint8_t>(m_buffer[base + 1]));
}

uint32_t EditableSliceFile::readWordBE(size_t offset) const {
    const size_t base = m_viewOffset + offset;
    return static_cast<uint32_t>((static_cast<uint8_t>(m_buffer[base]) << 24) |
                                 (static_cast<uint8_t>(m_buffer[base + 1]) << 16) |
                                 (static_cast<uint8_t>(m_buffer[base + 2]) << 8) |
                                 static_cast<uint8_t>(m_buffer[base + 3]));
}

bool EditableSliceFile::writeByte(size_t offset, uint8_t value) {
    if (offset >= m_viewLength) {
        return false;
    }
    m_buffer[m_viewOffset + offset] = static_cast<char>(value);
    m_dirty = true;
    return true;
}

bool EditableSliceFile::writeBytes(size_t offset, const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
        return true;
    }
    if (offset >= m_viewLength || offset + bytes.size() > m_viewLength) {
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        m_buffer[m_viewOffset + offset + i] = static_cast<char>(bytes[i]);
    }
    m_dirty = true;
    return true;
}

bool EditableSliceFile::insertBytes(size_t offset, const std::vector<uint8_t> &bytes) {
    if (bytes.empty()) {
        return true;
    }
    if (offset > m_viewLength) {
        return false;
    }
    size_t insertPos = m_viewOffset + offset;
    std::vector<char> charBytes(bytes.begin(), bytes.end());
    m_buffer.insert(m_buffer.begin() + static_cast<std::ptrdiff_t>(insertPos),
                    charBytes.begin(), charBytes.end());
    m_viewLength += bytes.size();
    m_dirty = true;
    return true;
}

bool EditableSliceFile::flush() {
    if (!m_dirty) {
        return true;
    }
    if (m_destPath.empty()) {
        L_ERROR("EditableSliceFile has no destination path");
        return false;
    }
    std::ofstream stream(m_destPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        L_ERROR("Failed to open {} for writing", m_destPath.string());
        return false;
    }
    stream.write(m_buffer.data(), static_cast<std::streamsize>(m_buffer.size()));
    if (!stream) {
        L_ERROR("Failed while writing {}", m_destPath.string());
        return false;
    }
    m_dirty = false;
    return true;
}
