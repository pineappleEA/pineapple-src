// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <utility>
#include "core/file_sys/vfs_vector.h"

namespace FileSys {
VectorVfsFile::VectorVfsFile(std::vector<u8> initial_data, std::string name_, VirtualDir parent_)
    : data(std::move(initial_data)), parent(std::move(parent_)), name(std::move(name_)) {}

VectorVfsFile::~VectorVfsFile() = default;

std::string VectorVfsFile::GetName() const {
    return name;
}

size_t VectorVfsFile::GetSize() const {
    return data.size();
}

bool VectorVfsFile::Resize(size_t new_size) {
    data.resize(new_size);
    return true;
}

VirtualDir VectorVfsFile::GetContainingDirectory() const {
    return parent;
}

bool VectorVfsFile::IsWritable() const {
    return true;
}

bool VectorVfsFile::IsReadable() const {
    return true;
}

std::size_t VectorVfsFile::Read(u8* data_, std::size_t length, std::size_t offset) const {
    const auto read = std::min(length, data.size() - offset);
    std::memcpy(data_, data.data() + offset, read);
    return read;
}

std::size_t VectorVfsFile::Write(const u8* data_, std::size_t length, std::size_t offset) {
    if (offset + length > data.size())
        data.resize(offset + length);
    const auto write = std::min(length, data.size() - offset);
    std::memcpy(data.data() + offset, data_, write);
    return write;
}

bool VectorVfsFile::Rename(std::string_view name_) {
    name = name_;
    return true;
}

void VectorVfsFile::Assign(std::vector<u8> new_data) {
    data = std::move(new_data);
}

VectorVfsDirectory::VectorVfsDirectory(std::vector<VirtualFile> files_,
                                       std::vector<VirtualDir> dirs_, std::string name_,
                                       VirtualDir parent_)
    : files(std::move(files_)), dirs(std::move(dirs_)), parent(std::move(parent_)),
      name(std::move(name_)) {}

VectorVfsDirectory::~VectorVfsDirectory() = default;

VirtualFile VectorVfsDirectory::GetFile(std::string_view file_name) const {
    if (!optimized_file_index_built) {
        optimized_file_index.clear();
        for (size_t i = 0; i < files.size(); i++) {
            optimized_file_index.emplace(files[i]->GetName(), i);
        }
        optimized_file_index_built = true;
    }

    const auto it = optimized_file_index.find(file_name);
    if (it != optimized_file_index.end()) {
        return files[it->second];
    }

    return nullptr;
}

std::vector<VirtualFile> VectorVfsDirectory::GetFiles() const {
    return files;
}

std::vector<VirtualDir> VectorVfsDirectory::GetSubdirectories() const {
    return dirs;
}

bool VectorVfsDirectory::IsWritable() const {
    return false;
}

bool VectorVfsDirectory::IsReadable() const {
    return true;
}

std::string VectorVfsDirectory::GetName() const {
    return name;
}

VirtualDir VectorVfsDirectory::GetParentDirectory() const {
    return parent;
}

template <typename T>
static bool FindAndRemoveVectorElement(std::vector<T>& vec, std::string_view name) {
    const auto iter =
        std::find_if(vec.begin(), vec.end(), [name](const T& e) { return e->GetName() == name; });
    if (iter == vec.end())
        return false;

    vec.erase(iter);
    return true;
}

bool VectorVfsDirectory::DeleteSubdirectory(std::string_view subdir_name) {
    return FindAndRemoveVectorElement(dirs, subdir_name);
}

bool VectorVfsDirectory::DeleteFile(std::string_view file_name) {
    optimized_file_index_built = false;
    return FindAndRemoveVectorElement(files, file_name);
}

bool VectorVfsDirectory::Rename(std::string_view name_) {
    name = name_;
    return true;
}

VirtualDir VectorVfsDirectory::CreateSubdirectory(std::string_view subdir_name) {
    return nullptr;
}

VirtualFile VectorVfsDirectory::CreateFile(std::string_view file_name) {
    return nullptr;
}

void VectorVfsDirectory::AddFile(VirtualFile file) {
    optimized_file_index_built = false;
    files.push_back(std::move(file));
}

void VectorVfsDirectory::AddDirectory(VirtualDir dir) {
    dirs.push_back(std::move(dir));
}
} // namespace FileSys
