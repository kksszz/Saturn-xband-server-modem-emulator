#pragma once
#include "../../include/xband/windows_image_storage.hpp"
// Compatibility aliases: existing Ymir integrations keep the same API.
namespace xband::ymir_adapter {
template<size_t N> using FixedImageStorage=xband::windows_storage::FixedImageStorage<N>;
using FlashStorage=xband::windows_storage::FlashStorage;
using VirtualCardStorage=xband::windows_storage::VirtualCardStorage;
}
