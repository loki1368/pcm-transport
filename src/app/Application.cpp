// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include "pcmtp/app/Application.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

#include "pcmtp/backend/AlsaPcmBackend.hpp"
#include "pcmtp/core/PlaybackEngine.hpp"
#include "pcmtp/decoder/FlacStreamDecoder.hpp"
#include "pcmtp/decoder/RangeLimitedDecoder.hpp"
#include "pcmtp/gui/GtkPlayerWindow.hpp"
#include "pcmtp/hardware/CardProfileRegistry.hpp"

namespace pcmtp {

int Application::run(int argc, char** argv) {
    std::string file_path;
    std::string device_name = "default";
    std::size_t transport_buffer_ms = 53;
    std::vector<std::string> gui_source_paths;
    bool probe_only = false;
    bool no_gui = false;
    bool positional_arguments = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (!positional_arguments && arg == "--") {
            positional_arguments = true;
        } else if (!positional_arguments && arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (!positional_arguments && arg == "--device" && i + 1 < argc) {
            device_name = argv[++i];
        } else if (!positional_arguments && arg == "--transport-buffer-ms" && i + 1 < argc) {
            transport_buffer_ms = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (!positional_arguments && arg == "--probe") {
            probe_only = true;
        } else if (!positional_arguments && arg == "--nogui") {
            no_gui = true;
        } else if (!positional_arguments && (arg == "--help" || arg == "-h")) {
            print_usage(argv[0]);
            return 0;
        } else if (!positional_arguments && !arg.empty() && arg[0] == '-') {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        } else {
            gui_source_paths.push_back(arg);
        }
    }

    if (probe_only && !gui_source_paths.empty()) {
        std::cerr << "File arguments cannot be combined with --probe\n";
        print_usage(argv[0]);
        return 1;
    }

    if (!file_path.empty() && !gui_source_paths.empty()) {
        std::cerr << "Positional file arguments cannot be combined with --file\n";
        print_usage(argv[0]);
        return 1;
    }

    if (probe_only) {
        return run_probe_only();
    }

    if (!file_path.empty()) {
        return run_player(file_path, device_name, transport_buffer_ms);
    }

    if (no_gui) {
        print_usage(argv[0]);
        return 1;
    }

    return run_gui(argv[0] != nullptr ? argv[0] : "pcm_transport",
                   gui_source_paths,
                   transport_buffer_ms);
}

int Application::run_probe_only() {
    const auto cards = CardProfileRegistry::probe_cards();
    if (cards.empty()) {
        std::cout << "No ALSA cards found.\n";
        return 0;
    }

    for (const auto& card : cards) {
        std::cout << "Card " << card.card_index << "\n";
        std::cout << "  short:   " << card.short_name << "\n";
        std::cout << "  long:    " << card.long_name << "\n";
        std::cout << "  hw:      " << card.hw_device << "\n";
        std::cout << "  plughw:  " << card.plughw_device << "\n";
        std::cout << "  alsa_hw_profile: " << card.alsa_hw_profile << "\n";
        std::cout << "  low-level features available: "
                  << (card.low_level_features_available ? "yes" : "no") << "\n";
        std::cout << '\n';
    }

    return 0;
}

int Application::run_player(const std::string& file_path, const std::string& device_name, std::size_t transport_buffer_ms) {
    std::unique_ptr<IAudioDecoder> decoder(new RangeLimitedDecoder(std::unique_ptr<IAudioDecoder>(new FlacStreamDecoder()), 0, 0));
    decoder->open(file_path);

    std::cout << "Opened FLAC: " << file_path << "\n";
    std::cout << "Format: " << decoder->format().to_string() << "\n";
    std::cout << "Device: " << device_name << "\n";
    std::cout << "ALSA transport buffer target: " << transport_buffer_ms << " ms\n\n";

    const auto cards = CardProfileRegistry::probe_cards();
    for (const auto& card : cards) {
        if (card.legacy_audigy_like) {
            std::cout << "Detected low-level ALSA mixer path on card " << card.card_index << "\n";
            std::cout << "Low-level DSP path remains reserved for a future version.\n\n";
        }
    }

    PlaybackEngine engine(transport_buffer_ms);
    engine.start(std::move(decoder), std::make_unique<AlsaPcmBackend>(), device_name);
    while (engine.is_playing()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!engine.last_error().empty()) {
        throw std::runtime_error(engine.last_error());
    }
    return 0;
}

int Application::run_gui(const std::string& program_name,
                         const std::vector<std::string>& source_paths,
                         std::size_t transport_buffer_ms) {
    GtkPlayerWindow window(transport_buffer_ms);
    window.show(program_name, source_paths);
    return 0;
}

void Application::print_usage(const char* program_name) const {
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << " [FILE...] [--transport-buffer-ms 120]\n";
    std::cout << "  " << program_name << " --file <path.flac> [--device default|hw:X,Y] [--transport-buffer-ms 120]\n";
    std::cout << "  " << program_name << " --probe\n";
}

} // namespace pcmtp
