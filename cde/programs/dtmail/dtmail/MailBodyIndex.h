#ifndef DTMAIL_MAIL_BODY_INDEX_H
#define DTMAIL_MAIL_BODY_INDEX_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>
#include <sys/stat.h>

struct sqlite3;

struct DtMailBodyHit {
    std::string path;
    std::string subject;
    std::string message_id;
    size_t ordinal;
    struct stat snapshot;

    bool stillCurrent() const;
};

struct DtMailBodySearch {
    bool ok = false;
    bool incomplete = false;
    std::vector<DtMailBodyHit> hits;
};

// A dialog keeps one private in-memory index. Only the search worker accesses it.
class DtMailBodyIndex {
public:
    DtMailBodyIndex() = default;
    ~DtMailBodyIndex();
    DtMailBodyIndex(const DtMailBodyIndex &) = delete;
    DtMailBodyIndex &operator=(const DtMailBodyIndex &) = delete;

    DtMailBodySearch search(const std::vector<std::string> &files,
                            const std::vector<std::string> &roots,
                            const std::string &query);

private:
    sqlite3 *_db = nullptr;
    std::map<std::string, struct stat> _indexed;

    bool initialize();
    bool erase(const std::string &path);
    bool refresh(const std::string &path);
};

#endif
