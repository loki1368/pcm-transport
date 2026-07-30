// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace pcmtp {

class Application {
public:
    int run(int argc, char** argv);

private:
    int run_probe_only();
    int run_player(const std::string& file_path, const std::string& device_name, std::size_t transport_buffer_ms);
    int run_gui(const std::string& program_name,
                const std::vector<std::string>& source_paths,
                std::size_t transport_buffer_ms);
    void print_usage(const char* program_name) const;
};

} // namespace pcmtp
