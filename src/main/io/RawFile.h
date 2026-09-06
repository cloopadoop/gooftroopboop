/*
 * VGMTrans (c) 2002-2019
 * Licensed under the zlib license,
 * refer to the included LICENSE.txt file
 */

#pragma once

#include <string_view>
#include <filesystem>
#include <vector>
#include <climits>
#include <cassert>
#include <variant>
#include "mio.hpp"

#include "Root.h"
#include "util/common.h"
#include "components/GTBTag.h"

class GTBFile;
class GTBItem;
class BytePattern;

class GTBSeq;
class GTBInstrSet;
class GTBSampColl;
class GTBMiscFile;

class RawFile {
   public:
    virtual ~RawFile() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    [[nodiscard]] virtual std::filesystem::path path() const = 0;
    [[nodiscard]] virtual size_t size() const noexcept = 0;
    [[nodiscard]] virtual std::string stem() const noexcept = 0;
    [[nodiscard]] virtual std::string extension() const = 0;

    [[nodiscard]] bool isValidOffset(uint32_t ofs) const noexcept { return ofs < size(); }

    [[nodiscard]] bool useLoaders() const noexcept { return m_flags & UseLoaders; }
    void setUseLoaders(bool enable) noexcept {
        if (enable) {
            m_flags |= UseLoaders;
        } else {
            m_flags &= ~UseLoaders;
        }
    }
    [[nodiscard]] bool useScanners() const noexcept { return m_flags & UseScanners; }
    void setUseScanners(bool enable) noexcept {
        if (enable) {
            m_flags |= UseScanners;
        } else {
            m_flags &= ~UseScanners;
        }
    }

    template <typename T>
    [[nodiscard]] T get(const size_t ind) const {
        assert(ind + sizeof(T) <= size());

        T value = 0;
        for (size_t i = 0; i < sizeof(T); i++) {
            value |= (static_cast<u8>(operator[](ind + i)) << (i * CHAR_BIT));
        }

        return value;
    }

    template <typename T>
    [[nodiscard]] T getBE(const size_t ind) const {
        assert(ind + sizeof(T) <= size());

        T value = 0u;
        for (size_t i = 0; i < sizeof(T); i++) {
            value |= (static_cast<u8>(operator[](ind + i)) << ((sizeof(T) - i - 1) * CHAR_BIT));
        }

        return value;
    }

    const char *begin() const noexcept { return data(); }
    const char *end() const noexcept { return data() + size(); }
    std::reverse_iterator<const char *> rbegin() const noexcept {
        return std::reverse_iterator<const char *>(end());
    }
    std::reverse_iterator<const char *> rend() const noexcept {
        return std::reverse_iterator<const char *>(begin());
    }
    virtual const char *data() const = 0;

    virtual const char &operator[](size_t i) const = 0;
    virtual uint8_t readByte(size_t offset) const = 0;
    virtual uint16_t readShort(size_t offset) const = 0;
    virtual uint32_t readWord(size_t offset) const = 0;
    virtual uint16_t readShortBE(size_t offset) const = 0;
    virtual uint32_t readWordBE(size_t offset) const = 0;
    std::string readNullTerminatedString(size_t offset, size_t maxLength) const;

    uint32_t readBytes(size_t offset, uint32_t nCount, void *pBuffer) const;
    bool matchBytes(const uint8_t *pattern, size_t offset, size_t nCount) const;
    bool matchBytePattern(const BytePattern &pattern, size_t offset) const;
    bool searchBytePattern(const BytePattern &pattern, uint32_t &nMatchOffset,
                           uint32_t nSearchOffset = 0, uint32_t nSearchSize = static_cast<uint32_t>(-1)) const;

    // Editing helpers. Override in writable RawFile implementations.
    [[nodiscard]] virtual bool isWritable() const { return false; }
    virtual bool writeByte(size_t /*offset*/, uint8_t /*value*/) { return false; }
    virtual bool writeBytes(size_t /*offset*/, const std::vector<uint8_t> & /*bytes*/) { return false; }
    virtual bool insertBytes(size_t /*offset*/, const std::vector<uint8_t> & /*bytes*/) { return false; }
    virtual bool flush() { return false; }

    [[nodiscard]] const auto &containedGTBFiles() const noexcept {
        return m_GTBfiles;
    }

    template <typename T>
    [[nodiscard]] const std::vector<T*> containedGTBFilesOfType() const noexcept {
      std::vector<T*> files = {};
      for (const auto& GTBfile : m_GTBfiles) {
        if (T* fileOfType = variantToType<T>(*GTBfile)) {
          files.emplace_back(fileOfType);
        }
      }
      return files;
    }
    void addContainedGTBFile(std::shared_ptr<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>>);
    void removeContainedGTBFile(std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>);

    GTBTag tag;

   private:
    std::vector<std::shared_ptr<std::variant<GTBSeq *, GTBInstrSet *, GTBSampColl *, GTBMiscFile *>>> m_GTBfiles;
    enum ProcessFlags { UseLoaders = 1, UseScanners = 2 };
    unsigned m_flags = UseLoaders | UseScanners;
};

class DiskFile final : public RawFile {
   public:
    DiskFile(const std::string &path);
    ~DiskFile() override = default;

    [[nodiscard]] std::string name() const override { return m_path.filename().string(); };
    [[nodiscard]] std::filesystem::path path() const override { return m_path; };
    [[nodiscard]] size_t size() const noexcept override { return m_data.length(); };
    [[nodiscard]] std::string stem() const noexcept override { return m_path.stem().string(); };
    [[nodiscard]] std::string extension() const override {
        auto tmp = m_path.extension().string();
        if (!tmp.empty()) {
            return toLower(tmp.substr(1, tmp.size() - 1));
        }

        return tmp;
    }

    const char *data() const override { return m_data.data(); }
    const char &operator[](size_t offset) const override { return m_data[offset]; }
    uint8_t readByte(size_t offset) const override { return m_data[offset]; }
    uint16_t readShort(size_t offset) const override { return get<u16>(offset); }
    uint32_t readWord(size_t offset) const override { return get<u32>(offset); }
    uint16_t readShortBE(size_t offset) const override { return getBE<u16>(offset); }
    uint32_t readWordBE(size_t offset) const override { return getBE<u32>(offset); }

   private:
    mio::mmap_source m_data;
    std::filesystem::path m_path;
};

class EditableRawFile final : public RawFile {
   public:
    explicit EditableRawFile(const std::string &path);
    ~EditableRawFile() override = default;

    [[nodiscard]] std::string name() const override { return m_path.filename().string(); };
    [[nodiscard]] std::filesystem::path path() const override { return m_path; };
    [[nodiscard]] size_t size() const noexcept override { return m_data.size(); };
    [[nodiscard]] std::string stem() const noexcept override { return m_path.stem().string(); };
    [[nodiscard]] std::string extension() const override {
        auto tmp = m_path.extension().string();
        if (!tmp.empty()) {
            return toLower(tmp.substr(1, tmp.size() - 1));
        }

        return tmp;
    }

    const char *data() const override { return m_data.data(); }
    const char &operator[](size_t offset) const override { return m_data[offset]; }
    uint8_t readByte(size_t offset) const override { return static_cast<uint8_t>(m_data[offset]); }
    uint16_t readShort(size_t offset) const override { return get<u16>(offset); }
    uint32_t readWord(size_t offset) const override { return get<u32>(offset); }
    uint16_t readShortBE(size_t offset) const override { return getBE<u16>(offset); }
    uint32_t readWordBE(size_t offset) const override { return getBE<u32>(offset); }

    [[nodiscard]] bool isWritable() const override { return true; }
    bool writeByte(size_t offset, uint8_t value) override;
    bool writeBytes(size_t offset, const std::vector<uint8_t> &bytes) override;
    bool flush() override;

   private:
    std::vector<char> m_data;
    std::filesystem::path m_path;
    bool m_dirty{false};
};

class VirtFile final : public RawFile {
   public:
    VirtFile() = default;
    VirtFile(const RawFile &, size_t offset = 0);
    VirtFile(const RawFile &, size_t offset, size_t limit);
    VirtFile(const uint8_t *data, uint32_t size, std::string name, std::string parent_fullpath = "",
             const GTBTag& tag = GTBTag());
    ~VirtFile() override = default;

    [[nodiscard]] std::string name() const override { return m_name; };
    [[nodiscard]] std::filesystem::path path() const override { return m_lpath; };
    [[nodiscard]] size_t size() const noexcept override {return m_data.size(); };
    [[nodiscard]] std::string stem() const noexcept override {
      auto tmp = m_lpath.stem();
      if (tmp.empty()) {
        std::filesystem::path tmp2(m_name);
        if (tmp2.has_filename()) {
          return tmp2.stem().string();
        }
      }

      return tmp.string();
    };
    [[nodiscard]] std::string extension() const override {
      std::filesystem::path tmp2(m_name);
      if (tmp2.has_extension()) {
        return toLower(tmp2.extension().string().substr(1, tmp2.extension().string().size() - 1));
      }
      auto tmp = m_lpath.extension().string();
      if (!tmp.empty()) {
        return toLower(tmp.substr(1, tmp.size() - 1));
      }
      return "";
    }

    const char *data() const override { return m_data.data(); }
    const char &operator[](size_t offset) const override { return m_data[offset]; }
    uint8_t readByte(size_t offset) const override { return m_data[offset]; }
    uint16_t readShort(size_t offset) const override { return get<u16>(offset); }
    uint32_t readWord(size_t offset) const override { return get<u32>(offset); }
    uint16_t readShortBE(size_t offset) const override { return getBE<u16>(offset); }
    uint32_t readWordBE(size_t offset) const override { return getBE<u32>(offset); }

   private:
    std::vector<char> m_data;
    std::string m_name;
    std::filesystem::path m_lpath;
};

// Writable view over a slice of a parent file; flush writes the entire parent buffer back to disk.
class EditableSliceFile final : public RawFile {
   public:
    EditableSliceFile(const RawFile &parent,
                      size_t sliceOffset,
                      size_t sliceLength,
                      std::string nameOverride = {},
                      const GTBTag &tag = GTBTag());
    ~EditableSliceFile() override = default;

    [[nodiscard]] std::string name() const override { return m_name; };
    [[nodiscard]] std::filesystem::path path() const override { return m_destPath; };
    [[nodiscard]] size_t size() const noexcept override { return m_viewLength; };
    [[nodiscard]] std::string stem() const noexcept override { return std::filesystem::path(m_name).stem().string(); };
    [[nodiscard]] std::string extension() const override {
        auto tmp = std::filesystem::path(m_name).extension().string();
        if (!tmp.empty() && tmp[0] == '.') {
            tmp.erase(0, 1);
        }
        return toLower(tmp);
    };

    const char *data() const override { return m_buffer.data() + m_viewOffset; }
    const char &operator[](size_t offset) const override { return m_buffer[m_viewOffset + offset]; }
    uint8_t readByte(size_t offset) const override { return static_cast<uint8_t>(m_buffer[m_viewOffset + offset]); }
    uint16_t readShort(size_t offset) const override;
    uint32_t readWord(size_t offset) const override;
    uint16_t readShortBE(size_t offset) const override;
    uint32_t readWordBE(size_t offset) const override;

    [[nodiscard]] bool isWritable() const override { return true; }
    bool writeByte(size_t offset, uint8_t value) override;
    bool writeBytes(size_t offset, const std::vector<uint8_t> &bytes) override;
    bool insertBytes(size_t offset, const std::vector<uint8_t> &bytes) override;
    bool flush() override;

    [[nodiscard]] size_t viewOffset() const { return m_viewOffset; }

   private:
    std::vector<char> m_buffer;
    size_t m_viewOffset{0};
    size_t m_viewLength{0};
    std::filesystem::path m_destPath;
    std::string m_name;
    bool m_dirty{false};
};
