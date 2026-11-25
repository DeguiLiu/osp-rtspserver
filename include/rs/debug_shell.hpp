// Telnet debug shell glue for the RTSP server (P0 read-only commands).
// SPDX-License-Identifier: MIT
#ifndef RS_DEBUG_SHELL_HPP_
#define RS_DEBUG_SHELL_HPP_

#include "rs/rtsp_server.hpp"

namespace rs {

#if defined(RS_ENABLE_DEBUG_SHELL)
// Binds the single RtspServer instance used by the shell command callbacks.
// Called from main before DebugShell::Start().
void BindDebugShell(RtspServer* server) noexcept;
#endif

}  // namespace rs

#endif  // RS_DEBUG_SHELL_HPP_
