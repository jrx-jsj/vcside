// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Juan Sánchez Jurado
// macOS: HTTPS through NSURLSession (the system's TLS, certificate store and proxy settings)
// and the key in the Keychain. Registered at start; other systems have no backend yet.
#pragma once

namespace vcs::assistant {
void registerAppleBackends();
}
