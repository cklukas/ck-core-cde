/* CardDAV collection snapshots used by dtmail recipient completion. */

#include "CardDavContacts.h"

#if DTMAIL_CARDDAV_ENABLED
#include "MailAccountStore.h"

#include <curl/curl.h>
#include <libxml/parser.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <future>
#include <memory>
#include <set>
#include <strings.h>
#include <thread>

namespace {
constexpr size_t response_limit = 12 * 1024 * 1024;
constexpr char dav[] = "DAV:";
constexpr char carddav[] = "urn:ietf:params:xml:ns:carddav";

bool named(xmlNode *node, const char *ns, const char *name)
{
    return node->type == XML_ELEMENT_NODE && node->ns &&
           xmlStrEqual(node->ns->href, BAD_CAST ns) &&
           xmlStrEqual(node->name, BAD_CAST name);
}

xmlNode *child(xmlNode *node, const char *ns, const char *name)
{
    for (xmlNode *item = node ? node->children : nullptr; item; item = item->next)
        if (named(item, ns, name))
            return item;
    return nullptr;
}

std::string content(xmlNode *node)
{
    if (!node)
        return {};
    xmlChar *raw = xmlNodeGetContent(node);
    std::string value = raw ? reinterpret_cast<const char *>(raw) : "";
    xmlFree(raw);
    return value;
}

char ascii_lower(unsigned char c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

bool contains(const char *value, const char *query)
{
    const char *end = value + strlen(value);
    return std::search(value, end, query, query + strlen(query),
                       [](char a, char b) { return ascii_lower(a) == ascii_lower(b); }) != end;
}

bool safe_email(const std::string &email)
{
    size_t at = email.find('@');
    if (email.size() > 254 || at == std::string::npos || at == 0 ||
        at + 1 == email.size() || email.find('@', at + 1) != std::string::npos)
        return false;
    for (unsigned char c : email)
        if (c <= ' ' || c >= 127 || strchr("()<>,;:\\\"[]", c))
            return false;
    return true;
}

std::string display_name(const std::string &raw)
{
    std::string name;
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = raw[i];
        if (c == '\\' && i + 1 < raw.size()) {
            c = raw[++i];
            if (c == 'n' || c == 'N')
                c = ' ';
        }
        // The compose recipient tokenizer treats these as address separators.
        if (c < ' ' || c == 127 || strchr("()<>,;:\\\"", c))
            c = ' ';
        name.push_back(c);
    }
    return name;
}

bool read_vcard(const std::string &card, std::vector<DtMailContact> &contacts,
                std::set<std::string> &seen)
{
    std::vector<std::string> lines;
    size_t start = 0;
    while (start < card.size()) {
        size_t end = card.find('\n', start);
        if (end == std::string::npos)
            end = card.size();
        size_t length = end - start;
        if (length && card[start + length - 1] == '\r')
            --length;
        if (length && (card[start] == ' ' || card[start] == '\t') && !lines.empty())
            lines.back().append(card, start + 1, length - 1);
        else
            lines.emplace_back(card, start, length);
        start = end + 1;
    }
    if (lines.size() < 2 || strcasecmp(lines.front().c_str(), "BEGIN:VCARD") ||
        strcasecmp(lines.back().c_str(), "END:VCARD"))
        return false;

    std::string name;
    std::vector<std::string> emails;
    for (const std::string &line : lines) {
        bool quoted = false;
        size_t colon = 0;
        for (; colon < line.size(); ++colon) {
            if (line[colon] == '"')
                quoted = !quoted;
            if (line[colon] == ':' && !quoted)
                break;
        }
        if (colon == line.size())
            continue;
        std::string property = line.substr(0, colon);
        property.resize(std::min(property.size(), property.find(';')));
        size_t group = property.rfind('.');
        if (group != std::string::npos)
            property.erase(0, group + 1);
        std::string value = line.substr(colon + 1);
        if (!strcasecmp(property.c_str(), "FN") && name.empty())
            name = display_name(value);
        if (!strcasecmp(property.c_str(), "EMAIL")) {
            if (!strncasecmp(value.c_str(), "mailto:", 7))
                value.erase(0, 7);
            if (safe_email(value))
                emails.push_back(value);
        }
    }
    for (const std::string &email : emails) {
        std::string key = email;
        std::transform(key.begin(), key.end(), key.begin(), ascii_lower);
        if (!seen.insert(key).second)
            continue;
        DtMailContact contact = {};
        if (name.size() < sizeof(contact.name) && name.size() + email.size() + 3 < 256)
            memcpy(contact.name, name.data(), name.size());
        memcpy(contact.email, email.data(), email.size());
        contacts.push_back(contact);
    }
    return true;
}

size_t receive(char *data, size_t size, size_t count, void *context)
{
    auto &response = *static_cast<std::string *>(context);
    size_t length = size * count;
    if (length > response_limit - response.size())
        return 0;
    try {
        response.append(data, length);
    } catch (...) {
        return 0;
    }
    return length;
}

struct Refresh {
    bool success = false;
    std::vector<DtMailContact> contacts;
};

struct Collection {
    std::string url, user, ca_file;

    bool operator==(const Collection &other) const
    {
        return url == other.url && user == other.user && ca_file == other.ca_file;
    }
};

Refresh fetch(const Collection &collection)
{
    Refresh result;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl)
        return result;

    constexpr char query[] =
        "<C:addressbook-query xmlns:C=\"urn:ietf:params:xml:ns:carddav\" xmlns:D=\"DAV:\">"
        "<D:prop><C:address-data/></D:prop><C:filter/></C:addressbook-query>";
    curl_slist *raw_headers = curl_slist_append(nullptr, "Depth: 1");
    if (!raw_headers)
        return result;
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(raw_headers, curl_slist_free_all);
    if (!curl_slist_append(raw_headers, "Content-Type: application/xml; charset=utf-8"))
        return result;
    std::string response;
    curl_easy_setopt(curl.get(), CURLOPT_URL, collection.url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl.get(), CURLOPT_DISALLOW_USERNAME_IN_URL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, "REPORT");
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, query);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 6L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    if (!collection.ca_file.empty() &&
        curl_easy_setopt(curl.get(), CURLOPT_CAINFO, collection.ca_file.c_str()) != CURLE_OK)
        return result;

    char password[1024] = {};
    if (!collection.user.empty()) {
        int rc = dtmail_keyring_lookup("carddav", "password", password, sizeof(password));
        curl_easy_setopt(curl.get(), CURLOPT_HTTPAUTH, CURLAUTH_ANY);
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, collection.user.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, password);
        for (volatile char &c : password)
            c = 0;
        if (rc != 0)
            return result;
    }
    long status = 0;
    if (curl_easy_perform(curl.get()) == CURLE_OK &&
        curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status) == CURLE_OK &&
        status == 207)
        result.success = dtmail_carddav_read(response, result.contacts);
    return result;
}

// Only the Xt thread accesses this state. The worker receives copies of its
// configuration and returns an independent snapshot.
using Clock = std::chrono::steady_clock;
Collection current, fetching;
Clock::time_point next_refresh;
std::future<Refresh> pending;
std::vector<DtMailContact> available;

void collect()
{
    if (!pending.valid() || pending.wait_for(std::chrono::seconds(0)) !=
                                std::future_status::ready)
        return;
    try {
        Refresh result = pending.get();
        if (current == fetching) {
            next_refresh = Clock::now() + std::chrono::seconds(result.success ? 300 : 30);
            if (result.success)
                available.swap(result.contacts);
        }
    } catch (...) {
        next_refresh = Clock::now() + std::chrono::seconds(30);
    }
}
} // namespace

bool dtmail_carddav_read(const std::string &xml, std::vector<DtMailContact> &contacts)
{
    if (xml.empty() || xml.size() > response_limit)
        return false;
    std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> doc(
        xmlReadMemory(xml.data(), static_cast<int>(xml.size()), nullptr, nullptr,
                      XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING),
        xmlFreeDoc);
    if (!doc || doc->intSubset || doc->extSubset)
        return false;
    xmlNode *root = xmlDocGetRootElement(doc.get());
    if (!root || !named(root, dav, "multistatus"))
        return false;

    std::vector<DtMailContact> fresh;
    std::set<std::string> seen;
    for (xmlNode *item = root->children; item; item = item->next) {
        if (!named(item, dav, "response"))
            continue;
        bool have_card = false;
        for (xmlNode *propstat = item->children; propstat; propstat = propstat->next) {
            if (!named(propstat, dav, "propstat"))
                continue;
            xmlNode *card = child(child(propstat, dav, "prop"), carddav, "address-data");
            if (!card)
                continue;
            std::string status = content(child(propstat, dav, "status"));
            int code = 0;
            if (sscanf(status.c_str(), "HTTP/%*s %d", &code) != 1 ||
                code < 200 || code >= 300 || !read_vcard(content(card), fresh, seen))
                return false;
            have_card = true;
        }
        if (!have_card)
            return false;
    }
    contacts.swap(fresh);
    return true;
}

void dtmail_carddav_refresh(const char *url, const char *username, const char *ca_file)
{
    Collection selected = {url ? url : "", username ? username : "", ca_file ? ca_file : ""};
    if (!(current == selected)) {
        current = std::move(selected);
        available.clear();
        next_refresh = Clock::time_point();
    }
    collect();
    if (pending.valid() || current.url.compare(0, 8, "https://") ||
        Clock::now() < next_refresh)
        return;
    static const bool curl_ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!curl_ready)
        return;
    fetching = current;
    try {
        std::packaged_task<Refresh()> task([collection = fetching] { return fetch(collection); });
        auto completion = task.get_future();
        // Closing the mailer does not wait for a keyring unlock or HTTPS reply.
        std::thread(std::move(task)).detach();
        pending = std::move(completion);
    } catch (...) {
        next_refresh = Clock::now() + std::chrono::seconds(30);
    }
}

int dtmail_carddav_match(const char *query, char results[][256], int capacity)
{
    if (!query || !*query || !results || capacity <= 0)
        return 0;
    collect();
    int count = 0;
    for (const DtMailContact &contact : available) {
        if (count == capacity)
            break;
        if (!contains(contact.name, query) && !contains(contact.email, query))
            continue;
        if (contact.name[0])
            snprintf(results[count++], 256, "%s <%s>", contact.name, contact.email);
        else
            snprintf(results[count++], 256, "%s", contact.email);
    }
    return count;
}

#else
bool dtmail_carddav_read(const std::string &, std::vector<DtMailContact> &) { return false; }
void dtmail_carddav_refresh(const char *, const char *, const char *) {}
int dtmail_carddav_match(const char *, char [][256], int) { return 0; }
#endif
