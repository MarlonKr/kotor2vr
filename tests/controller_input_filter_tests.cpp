#include "controller_input_filter.hpp"

#include <array>
#include <cstdlib>
#include <iostream>

using namespace k2vr::game32::controller;
namespace {
int failures{};
void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
ViewChordFilter Armed() { ViewChordFilter f; (void)f.Apply(0); return f; }
}

int main() {
    auto tap = Armed();
    Check(tap.Apply(kView).buttons == 0, "View press is deferred");
    for (int i=0;i<100;++i) Check(tap.Apply(kView).buttons == 0, "View hold never opens menu");
    auto released = tap.Apply(0);
    Check(released.buttons == kView && released.standalone_view_tap, "standalone release emits exactly one View press");
    Check(tap.Apply(0).buttons == 0, "following poll emits View release");
    Check(tap.Apply(0).buttons == 0, "tap cannot repeat");
    (void)tap.Apply(kView);
    Check(tap.Apply(0).standalone_view_tap, "second tap works");

    for (auto partner : std::array{kA,kB,kX,kY,kMenu,kDown}) {
        auto f = Armed();
        Check(f.Apply(partner).buttons == partner, "standalone partner passes through");
        (void)f.Apply(0);
        Check(f.Apply(kView).buttons == 0, "View first does not leak");
        Check(f.Apply(kView|partner).buttons == 0, "both chord members are consumed");
        Check(f.Apply(partner).buttons == 0, "partner stays consumed after View released");
        Check(f.Apply(partner).buttons == 0, "held partner does not reappear");
        Check(!f.Apply(0).standalone_view_tap, "chord release produces no standalone tap");
        Check(f.Apply(partner).buttons == partner, "fresh partner press works after release");

        auto reverse = Armed();
        (void)reverse.Apply(kView|partner);
        Check(reverse.Apply(kView).buttons == 0, "partner-first release keeps View consumed");
        Check(reverse.Apply(0).buttons == 0, "reverse release order does not leak a menu");
    }
    auto chain = Armed();
    (void)chain.Apply(kView|kY);
    (void)chain.Apply(kView);
    Check(chain.Apply(kView|kA|kDown).buttons == 0, "multiple/sequential partners consumed");
    Check(chain.Apply(kA|kDown).buttons == 0, "all held partners stay consumed");
    (void)chain.Apply(kDown);
    Check(chain.Apply(kA|kDown).buttons == kA, "released and repressed partner independent of another held partner");

    auto unrelated = Armed();
    constexpr auto shoulder = 1U << 4;
    Check(unrelated.Apply(kView|kY|shoulder).buttons == shoulder, "unrelated button preserved");
    auto pad1 = Armed(), pad2 = Armed();
    Check(pad1.Apply(kView).buttons == 0 && pad2.Apply(kY).buttons == kY, "pads cannot combine into a chord");
    Check(pad1.Apply(0).standalone_view_tap, "other pad does not cancel standalone View");

    auto reconnect = Armed();
    (void)reconnect.Apply(kView|kY);
    reconnect.Disconnect();
    Check(reconnect.Apply(kY).buttons == 0, "reconnect does not leak held chord partner");
    Check(reconnect.Apply(0).buttons == 0, "reconnect release does not synthesize tap");
    Check(reconnect.Apply(kY).buttons == kY, "new press after reconnect works");
    reconnect.Disconnect();
    Check(reconnect.Apply(kView).buttons == 0 && reconnect.Apply(0).buttons == 0, "View held across reconnect cannot create phantom menu");
    (void)reconnect.Apply(kView);
    Check(reconnect.Apply(0).standalone_view_tap, "standalone tap works after reconnect neutral");

    Check(!PovHasDown(0xFFFFFFFFU) && !PovHasDown(0xFFFFU), "both neutral POV forms pass");
    Check(PovHasDown(13500) && PovHasDown(18000) && PovHasDown(22500), "down including diagonals recognized");
    Check(!PovHasDown(0) && !PovHasDown(9000) && !PovHasDown(27000), "other POV directions not consumed");
    std::cout << (failures ? "FAIL" : "PASS") << ": controller View/chord filter\n";
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
