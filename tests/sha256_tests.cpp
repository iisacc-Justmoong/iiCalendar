#include "../src/Security/Sha256.h"
#include <iostream>
#include <span>
#include <string_view>

int main() {
    using iiCalendar::detail::Sha256;
    auto bytes = [](std::string_view value) { return std::as_bytes(std::span(value)); };
    Sha256 hash;
    if (hash.hexDigest() != "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") return 1;
    hash.update(bytes("a"));
    hash.update(bytes("bc"));
    if (hash.hexDigest() != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") return 2;
    const auto message = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    if (iiCalendar::detail::sha256(bytes(message)) != "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") return 3;
    std::cout << "SHA-256 known vectors passed\n";
}
