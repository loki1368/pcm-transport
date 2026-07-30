// SPDX-FileCopyrightText: 2026 Andrey Berestov and PCM Transport contributors
// SPDX-License-Identifier: GPL-3.0-only

#include <exception>
#include <iostream>

#include "pcmtp/app/Application.hpp"

int main(int argc, char** argv) {
    try {
        pcmtp::Application app;
        return app.run(argc, argv);
    } catch (const std::exception& ex) {
        std::cerr << "Error: " << ex.what() << '\n';
        return 1;
    }
}
