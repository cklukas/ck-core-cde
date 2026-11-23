#include "MailBodyIndex.h"
#include "MailSignature.h"

#include <algorithm>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

int main(int argc, char **argv)
{
    assert(argc == 3);
    if (std::string(argv[1]) == "verify") {
        std::ifstream input(argv[2], std::ios::binary);
        assert(input);
        std::string bytes{std::istreambuf_iterator<char>(input), {}};
        DtMailSignature result = dtmail_verify_signature(bytes.data(), bytes.size());
        std::cout << static_cast<int>(result.kind) << ' ' << static_cast<int>(result.status) << '\n';
        return 0;
    }

    DtMailBodyIndex index;
    std::string root = argv[2];
    auto search = [&](const char *query) { return index.search({}, {root}, query); };
    auto result = search("shared phrase");
    assert(result.ok && !result.incomplete && result.hits.size() == 2);
    assert(result.hits[0].path != result.hits[1].path);
    auto subject = std::find_if(result.hits.begin(), result.hits.end(),
        [](const auto &hit) { return hit.message_id == "quoted@example.test"; });
    assert(subject != result.hits.end() && subject->subject == "Plain" && subject->ordinal == 1);
    assert(subject->stillCurrent());
    assert(search("cafe").hits.size() == 1);
    assert(search("attachmenttoken").hits.empty());
    assert(search("scripttoken").hits.empty());
    auto syntax = search("\" OR *");
    assert(syntax.ok && syntax.hits.empty());

    std::ofstream(subject->path, std::ios::app)
        << "\nFrom fixture Mon Nov 23 10:00:01 2026\nSubject: Added\n"
           "Message-ID: <appended@example.test>\n\nnew body addition\n";
    assert(!subject->stillCurrent());
    result = search("body addition");
    assert(result.ok && result.hits.size() == 1 &&
           result.hits[0].message_id == "appended@example.test" && result.hits[0].ordinal == 2);
    auto added = result.hits[0];
    std::filesystem::remove(added.path);
    assert(!added.stillCurrent());
    result = search("shared phrase");
    assert(result.ok && result.hits.size() == 1);
}
