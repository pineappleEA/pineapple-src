// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "core/file_sys/vfs.h"

namespace FileSys {

// An implementation of VfsFile that is backed by a statically-sized array
template <std::size_t size>
class ArrayVfsFile : public VfsFile {
public:
    explicit ArrayVfsFile(const std::array<u8, size>& data_, std::string name_ = "",
                          VirtualDir parent_ = nullptr)
        : data(data_), name(std::move(name_)), parent(std::move(parent_)) {}

    std::string GetName() const override {
        return name;
    }

    std::size_t GetSize() const override {
        return size;
    }

    bool Resize(std::size_t new_size) override {
        return false;
    }

    VirtualDir GetContainingDirectory() const override {
        return parent;
    }

    bool IsWritable() const override {
        return false;
    }

    bool IsReadable() const override {
        return true;
    }

    std::size_t Read(u8* data_, std::size_t length, std::size_t offset) const override {
        const auto read = std::min(length, size - offset);
        std::memcpy(data_, data.data() + offset, read);
        return read;
    }

    std::size_t Write(const u8* data_, std::size_t length, std::size_t offset) override {
        return 0;
    }

    bool Rename(std::string_view new_name) override {
        name = new_name;
        return true;
    }

private:
    std::array<u8, size> data;
    std::string name;
    VirtualDir parent;
};

template <std::size_t Size, typename... Args>
std::shared_ptr<ArrayVfsFile<Size>> MakeArrayFile(const std::array<u8, Size>& data,
                                                  Args&&... args) {
    return std::make_shared<ArrayVfsFile<Size>>(data, std::forward<Args>(args)...);
}

// An implementation of VfsFile that is backed by a vector optionally supplied upon construction
class VectorVfsFile : public VfsFile {
public:
    explicit VectorVfsFile(std::vector<u8> initial_data = {}, std::string name_ = "",
                           VirtualDir parent_ = nullptr);
    ~VectorVfsFile() override;

    std::string GetName() const override;
    std::size_t GetSize() const override;
    bool Resize(std::size_t new_size) override;
    VirtualDir GetContainingDirectory() const override;
    bool IsWritable() const override;
    bool IsReadable() const override;
    std::size_t Read(u8* data, std::size_t length, std::size_t offset) const override;
    std::size_t Write(const u8* data, std::size_t length, std::size_t offset) override;
    bool Rename(std::string_view name) override;

    virtual void Assign(std::vector<u8> new_data);

private:
    std::vector<u8> data;
    VirtualDir parent;
    std::string name;
};

// An implementation of VfsDirectory that maintains two vectors for subdirectories and files.
// Vector data is supplied upon construction.
class VectorVfsDirectory : public VfsDirectory {
public:
    explicit VectorVfsDirectory(std::vector<VirtualFile> files = {},
                                std::vector<VirtualDir> dirs = {}, std::string name = "",
                                VirtualDir parent = nullptr);
    ~VectorVfsDirectory() override;

    VirtualFile GetFile(std::string_view file_name) const override;
    std::vector<VirtualFile> GetFiles() const override;
    std::vector<VirtualDir> GetSubdirectories() const override;
    bool IsWritable() const override;
    bool IsReadable() const override;
    std::string GetName() const override;
    VirtualDir GetParentDirectory() const override;
    bool DeleteSubdirectory(std::string_view subdir_name) override;
    bool DeleteFile(std::string_view file_name) override;
    bool Rename(std::string_view name) override;
    VirtualDir CreateSubdirectory(std::string_view subdir_name) override;
    VirtualFile CreateFile(std::string_view file_name) override;

    virtual void AddFile(VirtualFile file);
    virtual void AddDirectory(VirtualDir dir);

private:
    std::vector<VirtualFile> files;
    std::vector<VirtualDir> dirs;

    VirtualDir parent;
    std::string name;

    mutable std::map<std::string, size_t, std::less<>> optimized_file_index;
    mutable bool optimized_file_index_built{};
};

} // namespace FileSys
