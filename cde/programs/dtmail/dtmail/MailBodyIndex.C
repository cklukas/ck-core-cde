#include "MailBodyIndex.h"

#if DTMAIL_BODY_INDEX_ENABLED
#include "HtmlReadable.h"
#include "MailSignature.h"

#include <gmime/gmime.h>
#include <sqlite3.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <set>
#include <fcntl.h>
#include <unistd.h>

namespace {
constexpr size_t hit_limit = 500;

bool same_file(const struct stat &a, const struct stat &b)
{
#ifdef __APPLE__
    const auto &am = a.st_mtimespec;
    const auto &bm = b.st_mtimespec;
    const auto &ac = a.st_ctimespec;
    const auto &bc = b.st_ctimespec;
#else
    const auto &am = a.st_mtim;
    const auto &bm = b.st_mtim;
    const auto &ac = a.st_ctim;
    const auto &bc = b.st_ctim;
#endif
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
           am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec &&
           ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

Statement prepare(sqlite3 *db, const char *sql)
{
    sqlite3_stmt *stmt = nullptr;
    sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
    return Statement(stmt, sqlite3_finalize);
}

bool exec(sqlite3 *db, const char *sql)
{
    return sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

bool bind(sqlite3_stmt *stmt, int column, const std::string &value)
{
    return sqlite3_bind_text64(stmt, column, value.data(), value.size(), SQLITE_TRANSIENT,
                               SQLITE_UTF8) == SQLITE_OK;
}

std::string column(sqlite3_stmt *stmt, int index)
{
    const auto *value = sqlite3_column_text(stmt, index);
    return value ? reinterpret_cast<const char *>(value) : "";
}

bool is_mbox(const std::string &path)
{
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    struct stat st = {};
    char marker[5];
    bool mbox = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
                (st.st_size == 0 ||
                 (read(fd, marker, sizeof(marker)) == sizeof(marker) && !memcmp(marker, "From ", 5)));
    close(fd);
    return mbox;
}

std::set<std::string> collect_folders(const std::vector<std::string> &files,
                                      const std::vector<std::string> &roots)
{
    namespace fs = std::filesystem;
    std::set<std::string> paths;
    for (const auto &file : files) {
        std::error_code ec;
        auto path = fs::canonical(file, ec);
        if (!ec && is_mbox(path.string()))
            paths.insert(path.string());
    }
    for (const auto &root : roots) {
        std::error_code ec;
        auto full = fs::canonical(root, ec);
        if (ec || full == full.root_path())
            continue;
        auto iter = fs::recursive_directory_iterator(full,
            fs::directory_options::skip_permission_denied, ec);
        for (; !ec && iter != fs::recursive_directory_iterator(); iter.increment(ec)) {
            const auto &entry = *iter;
            std::error_code stat_error;
            if (fs::is_regular_file(entry.symlink_status(stat_error)) &&
                is_mbox(entry.path().string()))
                paths.insert(entry.path().string());
        }
    }
    return paths;
}

bool type_is(GMimeObject *object, const char *type, const char *subtype)
{
    return object && g_mime_content_type_is_type(
        g_mime_object_get_content_type(object), type, subtype);
}

void append_body(GMimeObject *part, std::string &body, unsigned depth = 0)
{
    if (!part || depth == 64)
        return;
    const char *disposition = g_mime_object_get_disposition(part);
    if (disposition && !g_ascii_strcasecmp(disposition, GMIME_DISPOSITION_ATTACHMENT))
        return;
    if (GMIME_IS_MULTIPART(part)) {
        auto *multipart = GMIME_MULTIPART(part);
        int count = g_mime_multipart_get_count(multipart);
        if (GMIME_IS_MULTIPART_SIGNED(part))
            count = count ? 1 : 0;
        if (type_is(part, "multipart", "related"))
            count = count ? 1 : 0;
        if (type_is(part, "multipart", "alternative")) {
            for (int i = 0; i < count; ++i) {
                GMimeObject *child = g_mime_multipart_get_part(multipart, i);
                if (type_is(child, "text", "plain")) {
                    append_body(child, body, depth + 1);
                    return;
                }
            }
        }
        for (int i = 0; i < count; ++i)
            append_body(g_mime_multipart_get_part(multipart, i), body, depth + 1);
        return;
    }
    if (!GMIME_IS_TEXT_PART(part) || g_mime_part_get_filename(GMIME_PART(part)))
        return;
    bool html = type_is(part, "text", "html");
    if (!html && !type_is(part, "text", "plain"))
        return;
    std::unique_ptr<char, decltype(&g_free)> text(
        g_mime_text_part_get_text(GMIME_TEXT_PART(part)), g_free);
    if (!text)
        return;
    std::unique_ptr<char, decltype(&free)> readable(
        html ? dtmail_html_to_readable_text(text.get(), strlen(text.get())) : nullptr, free);
    const char *decoded = html ? readable.get() : text.get();
    if (decoded) {
        if (!body.empty())
            body += '\n';
        body += decoded;
    }
}

struct Unref {
    void operator()(gpointer object) const { g_object_unref(object); }
};
template<class T> using GObjectPtr = std::unique_ptr<T, Unref>;
} // namespace

bool DtMailBodyHit::stillCurrent() const
{
    struct stat current = {};
    return lstat(path.c_str(), &current) == 0 && same_file(snapshot, current);
}

DtMailBodyIndex::~DtMailBodyIndex()
{
    sqlite3_close(_db);
}

bool DtMailBodyIndex::initialize()
{
    if (_db)
        return true;
    if (sqlite3_open(":memory:", &_db) == SQLITE_OK &&
        exec(_db, "PRAGMA temp_store=MEMORY") &&
        exec(_db, "CREATE VIRTUAL TABLE bodies USING fts5("
                  "body, path UNINDEXED, subject UNINDEXED, message_id UNINDEXED, ordinal UNINDEXED)"))
        return true;
    sqlite3_close(_db);
    _db = nullptr;
    return false;
}

bool DtMailBodyIndex::erase(const std::string &path)
{
    auto stmt = prepare(_db, "DELETE FROM bodies WHERE path = ?1");
    return stmt && bind(stmt.get(), 1, path) && sqlite3_step(stmt.get()) == SQLITE_DONE;
}

bool DtMailBodyIndex::refresh(const std::string &path)
{
    struct stat before = {}, after = {};
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return false;
    if (fstat(fd, &before) != 0 || !S_ISREG(before.st_mode)) {
        close(fd);
        return false;
    }
    auto old = _indexed.find(path);
    if (old != _indexed.end() && same_file(old->second, before)) {
        close(fd);
        return true;
    }

    // The stream owns the descriptor, including on a parse or SQL error.
    GObjectPtr<GMimeStream> stream(g_mime_stream_fs_new(fd));
    g_mime_stream_fs_set_owner(GMIME_STREAM_FS(stream.get()), true);
    GObjectPtr<GMimeParser> parser(g_mime_parser_new_with_stream(stream.get()));
    g_mime_parser_set_format(parser.get(), GMIME_FORMAT_MBOX);
    g_mime_parser_set_respect_content_length(parser.get(), true);
    if (!exec(_db, "BEGIN"))
        return false;
    auto insert = prepare(_db, "INSERT INTO bodies(body,path,subject,message_id,ordinal) "
                               "VALUES(?1,?2,?3,?4,?5)");
    bool ok = insert && erase(path);
    size_t ordinal = 0;
    while (ok && before.st_size > 0 && !g_mime_parser_eos(parser.get())) {
        GObjectPtr<GMimeMessage> message(g_mime_parser_construct_message(parser.get(), nullptr));
        if (!message) {
            ok = g_mime_parser_eos(parser.get());
            break;
        }
        ++ordinal;
        std::string body;
        append_body(g_mime_message_get_mime_part(message.get()), body);
        if (body.empty())
            continue;
        const char *subject = g_mime_message_get_subject(message.get());
        const char *id = g_mime_message_get_message_id(message.get());
        sqlite3_stmt *stmt = insert.get();
        ok = sqlite3_reset(stmt) == SQLITE_OK && bind(stmt, 1, body) && bind(stmt, 2, path) &&
             bind(stmt, 3, subject ? subject : "") && bind(stmt, 4, id ? id : "") &&
             sqlite3_bind_int64(stmt, 5, ordinal) == SQLITE_OK && sqlite3_step(stmt) == SQLITE_DONE;
    }
    if (ok && fstat(fd, &after) == 0 && same_file(before, after) &&
        lstat(path.c_str(), &after) == 0 && same_file(before, after) && exec(_db, "COMMIT")) {
        _indexed[path] = before;
        return true;
    }
    exec(_db, "ROLLBACK");
    return false;
}

DtMailBodySearch DtMailBodyIndex::search(const std::vector<std::string> &files,
                                         const std::vector<std::string> &roots,
                                         const std::string &query)
{
    DtMailBodySearch result;
    if (!initialize() || query.empty())
        return result;
    dtmail_init_mime();
    auto live = collect_folders(files, roots);
    for (auto it = _indexed.begin(); it != _indexed.end();) {
        if (live.count(it->first))
            ++it;
        else if (erase(it->first))
            it = _indexed.erase(it);
        else
            return result;
    }
    for (const auto &path : live) {
        if (refresh(path))
            continue;
        result.incomplete = true;
        if (!erase(path))
            return result;
        _indexed.erase(path);
    }

    // An input phrase is data. Escape FTS syntax even though the SQL is bound.
    std::string phrase = "\"";
    for (char c : query) {
        phrase += c;
        if (c == '"')
            phrase += '"';
    }
    phrase += '"';
    auto stmt = prepare(_db, "SELECT path, subject, message_id, ordinal FROM bodies "
                             "WHERE bodies MATCH ?1 ORDER BY rank LIMIT ?2");
    if (!stmt || !bind(stmt.get(), 1, phrase) ||
        sqlite3_bind_int64(stmt.get(), 2, hit_limit + 1) != SQLITE_OK)
        return result;

    int status;
    while ((status = sqlite3_step(stmt.get())) == SQLITE_ROW) {
        std::string path = column(stmt.get(), 0);
        const auto &snapshot = _indexed.at(path);
        result.hits.push_back({path, column(stmt.get(), 1), column(stmt.get(), 2),
            static_cast<size_t>(sqlite3_column_int64(stmt.get(), 3)), snapshot});
    }
    result.ok = status == SQLITE_DONE;
    if (result.hits.size() > hit_limit) {
        result.hits.pop_back();
        result.incomplete = true;
    }
    return result;
}

#else
bool DtMailBodyHit::stillCurrent() const { return false; }
DtMailBodyIndex::~DtMailBodyIndex() = default;
DtMailBodySearch DtMailBodyIndex::search(const std::vector<std::string> &,
                                         const std::vector<std::string> &,
                                         const std::string &)
{
    return {};
}
#endif
