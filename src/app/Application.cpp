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
    bool probe_only = false;
    bool no_gui = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--file" && i + 1 < argc) {
            file_path = argv[++i];
        } else if (arg == "--device" && i + 1 < argc) {
            device_name = argv[++i];
        } else if (arg == "--transport-buffer-ms" && i + 1 < argc) {
            transport_buffer_ms = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--probe") {
            probe_only = true;
        } else if (arg == "--nogui") {
            no_gui = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            print_usage(argv[0]);
            return 1;
        }
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

    return run_gui(argc, argv, transport_buffer_ms);
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

int Application::run_gui(int, char**, std::size_t transport_buffer_ms) {
    GtkPlayerWindow window(transport_buffer_ms);
    window.show();
    return 0;
}

void Application::print_usage(const char* program_name) const {
    std::cout << "Usage:\n";
    std::cout << "  " << program_name << "                        # start GTK UI\n";
    std::cout << "  " << program_name << " --file <path.flac> [--device default|hw:X,Y] [--transport-buffer-ms 120]\n";
    std::cout << "  " << program_name << " --probe\n";
}

} // namespace pcmtp
