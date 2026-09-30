// A fake ingest host driven by stdin. "FAKE exit 3" exits 3, "FAKE crash" crashes,
// "FAKE sleep" stalls, "FAKE garbage" writes non-JSON, "FAKE partial" writes a page with no
// lines, "FAKE huge" exceeds any cap and "FAKE echo" reports the bytes received. Anything else
// gets two canned pages
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace {

const char* const kPages =
    R"({"pages":[{"width":595,"height":842,"rotation":0,"images":0,"lines":[)"
    R"({"text":"1.1 Offer allopurinol after a first attack.","box":[72,72,400,84]},)"
    R"({"text":"1.2 Check urate six weeks after any dose change.","box":[72,100,560,112]}]},)"
    R"({"width":595,"height":842,"rotation":0,"images":1,"lines":[]}]})";

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2 || std::strcmp(argv[1], "extract") != 0) return 1;
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    std::vector<char> bytes;
    char buffer[1 << 16];
    for (;;) {
        const auto count = std::fread(buffer, 1, sizeof buffer, stdin);
        if (count == 0) break;
        bytes.insert(bytes.end(), buffer, buffer + count);
    }
    const std::string head(bytes.begin(), bytes.begin() + std::min<std::size_t>(bytes.size(), 16));
    if (head.rfind("FAKE exit ", 0) == 0) return std::atoi(head.c_str() + 10);
    if (head.rfind("FAKE crash", 0) == 0) TerminateProcess(GetCurrentProcess(), 0xC0000005);
    if (head.rfind("FAKE sleep", 0) == 0) Sleep(30'000);
    if (head.rfind("FAKE garbage", 0) == 0) {
        std::fputs("not json at all", stdout);
        return 0;
    }
    if (head.rfind("FAKE partial", 0) == 0) {
        std::fputs(R"({"pages":[{"width":100,"height":100}]})", stdout);
        return 0;
    }
    if (head.rfind("FAKE huge", 0) == 0) {
        const std::string block(1 << 16, 'x');
        for (int i = 0; i < 64; ++i) std::fwrite(block.data(), 1, block.size(), stdout);
        return 0;
    }
    if (head.rfind("FAKE echo", 0) == 0) {
        std::printf(R"({"pages":[{"width":100,"height":100,"rotation":0,"images":0,)"
                    R"("lines":[{"text":"%zu bytes","box":[0,0,50,10]}]}]})",
                    bytes.size());
        return 0;
    }
    std::fputs(kPages, stdout);
    std::fprintf(stderr, "fake-ingest-host: 2 pages\n");
    return 0;
}
