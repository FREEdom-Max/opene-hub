#pragma once

#include "GaokunKeyboard.h"

#include "shared/Channel.h"

// 键盘通道的名字。线路格式由 shared/Channel.h 的模板决定，与笔完全同构。
namespace Gaokun::Keyboard::Wire {

#if defined(_M_X64) && !defined(_M_ARM64EC)
inline constexpr wchar_t kSnapshotName[] = L"Local\\openehubKbdSnapshot";
inline constexpr wchar_t kEventPipeName[] = L"\\\\.\\pipe\\openehubKbdEvents";
inline constexpr wchar_t kCommandPipeName[] = L"\\\\.\\pipe\\openehubKbdCommands";
inline constexpr wchar_t kStopEventPrefix[] = L"openehubKbdHostStop";
#else
inline constexpr wchar_t kSnapshotName[] = L"Global\\GaokunKbdSnapshot";
inline constexpr wchar_t kEventPipeName[] = L"\\\\.\\pipe\\GaokunKbdEvents";
inline constexpr wchar_t kCommandPipeName[] = L"\\\\.\\pipe\\GaokunKbdCommands";
inline constexpr wchar_t kStopEventPrefix[] = L"GaokunKbdHostStop";
#endif

using SnapshotWriter = Channel::SeqlockWriter<Snapshot>;
using SnapshotReaderImpl = Channel::SeqlockReader<Snapshot>;
using EventWriter = Channel::EventPipeWriter<Event>;
using EventReaderImpl = Channel::EventPipeReader<Event>;
using CommandReader = Channel::CommandPipeReader<Command>;
using CommandWriterImpl = Channel::CommandPipeWriter<Command>;

} // namespace Gaokun::Keyboard::Wire
