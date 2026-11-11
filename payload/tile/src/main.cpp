/*
 * PS5 A2UI's home-screen tile.
 * Copyright (C) 2026 Laptopmini
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A native title (registered by ShadowMountPlus from /data/homebrew/PPSA95210) whose only job is
 * to start the real app: it asks websrv on this console to launch
 * /data/homebrew/PS5AgenticUI/eboot.elf with its homebrew loader, the same request
 * homebrew.js and scripts/ps5-run.sh make. websrv then closes this title and runs the app in its
 * place. Native titles can't read /data and lack the full C++ runtime the app needs, which is why
 * the app itself stays a payload.
 *
 * Built with drakmor/ps5-native-app-boilerplate (GPL-3.0-or-later): its startup code is linked
 * in, so this program is GPL-3.0-or-later. The rest of the repository is not.
 * If websrv isn't running, or the app isn't installed, this shows what to do and stays open.
 */

#include "demo_renderer.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <string_view>

namespace
{
constexpr std::string_view kRequest =
    "GET /hbldr?path=/data/homebrew/PS5AgenticUI/eboot.elf&cwd=/data/homebrew/PS5AgenticUI"
    "&env=LD_LIBRARY_PATH=/data/homebrew/PS5AgenticUI HTTP/1.0\r\n"
    "Host: 127.0.0.1\r\n\r\n";

// What the screen says when the hand-off fails (the bitmap font is upper case).
std::string_view line1 = "PS5 A2UI";
std::string_view line2;
std::string_view line3;

void draw_message(ps5::demo::Canvas &canvas) noexcept
{
    using ps5::demo::Color;
    canvas.clear(Color::background);
    canvas.text(120, 120, line1, 10, Color::white);
    canvas.rectangle(120, 260, 1680, 6, Color::white);
    canvas.text(120, 360, line2, 5, Color::cyan);
    canvas.text(120, 460, line3, 4, Color::white);
}

enum class Handoff
{
    no_websrv,
    refused,
    pending,
};

Handoff ask_websrv() noexcept
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return Handoff::no_websrv;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(8080);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    timeval timeout{20, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0)
    {
        (void)close(fd);
        return Handoff::no_websrv;
    }
    for (std::size_t sent = 0; sent < kRequest.size();)
    {
        const auto n = send(fd, kRequest.data() + sent, kRequest.size() - sent, 0);
        if (n <= 0)
        {
            (void)close(fd);
            return Handoff::no_websrv;
        }
        sent += static_cast<std::size_t>(n);
    }
    // websrv closes this title before it answers, so normally nothing below runs. An answer
    // means it couldn't launch the app (503: the ELF is missing or failed to load).
    std::array<char, 64> reply{};
    const auto n = recv(fd, reply.data(), reply.size() - 1, 0);
    (void)close(fd);
    if (n > 12 && std::memcmp(reply.data() + 9, "200", 3) != 0)
    {
        return Handoff::refused;
    }
    return Handoff::pending;
}
} // namespace

int main()
{
    switch (ask_websrv())
    {
    case Handoff::no_websrv:
        line2 = "WEBSRV IS NOT RUNNING";
        line3 = "START IT IN PAYLOAD MANAGER, OR ADD IT TO AUTOLOAD, THEN OPEN THIS AGAIN";
        ps5::demo::run(draw_message, "PS5 A2UI needs websrv: start it in Payload Manager.");
    case Handoff::refused:
        line2 = "THE APP IS NOT INSTALLED";
        line3 = "INSTALL IT IN /DATA/HOMEBREW/PS5AGENTICUI (SCRIPTS/PS5-INSTALL.SH)";
        ps5::demo::run(draw_message, "PS5 A2UI isn't installed in /data/homebrew/PS5AgenticUI.");
    case Handoff::pending:
        line2 = "STARTING";
        line3 = "IF NOTHING HAPPENS, CLOSE THIS AND CHECK WEBSRV";
        ps5::demo::run(draw_message, "Starting PS5 A2UI…");
    }
}
