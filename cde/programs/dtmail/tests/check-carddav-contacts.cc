#include "CardDavContacts.h"

#include <cassert>
#include <chrono>
#include <cstring>
#include <thread>

int main(int argc, char **argv)
{
    if (argc == 4) {
        bool should_fetch = !strcmp(argv[3], "trusted");
        dtmail_carddav_refresh(argv[1], "fixture", should_fetch ? argv[2] : nullptr);
        for (int i = 0; i < (should_fetch ? 100 : 20); ++i) {
            char results[2][256];
            int count = dtmail_carddav_match("example.test", results, 2);
            if (count) {
                assert(should_fetch && count == 1);
                assert(!strcmp(results[0], "Remote Contact <remote@example.test>"));
                dtmail_carddav_refresh(nullptr, nullptr, nullptr);
                assert(dtmail_carddav_match("example.test", results, 2) == 0);
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        assert(!should_fetch);
        return 0;
    }
    const std::string opening =
        "<D:multistatus xmlns:D='DAV:' xmlns:C='urn:ietf:params:xml:ns:carddav'>";
    const std::string response =
        "<D:response><D:href>/book/test.vcf</D:href><D:propstat><D:prop><C:address-data>";
    const std::string ending =
        "</C:address-data></D:prop><D:status>HTTP/1.1 200 OK</D:status>"
        "</D:propstat></D:response>";
    const std::string closing = "</D:multistatus>";
    std::vector<DtMailContact> contacts;
    assert(dtmail_carddav_read(opening + response +
        "BEGIN:VCARD\r\nVERSION:4.0\r\nFN:Zoë\\, García\r\n"
        "item.EMAIL;TYPE=\"work:primary\":zoe@\r\n example.test\r\n"
        "EMAIL:ZOe@example.test\r\nEMAIL;TYPE=home:mailto:z@home.test\r\n"
        "EMAIL:bad@example.test,other@example.test\r\nEND:VCARD\r\n" +
        ending + closing, contacts));
    assert(contacts.size() == 2);
    assert(!strcmp(contacts[0].name, "Zoë  García"));
    assert(!strcmp(contacts[0].email, "zoe@example.test"));
    assert(!strcmp(contacts[1].email, "z@home.test"));

    assert(!dtmail_carddav_read(opening + response + "not a vcard" + ending + closing, contacts));
    assert(!dtmail_carddav_read(opening + "<D:response><D:status>HTTP/1.1 507 Insufficient Storage"
        "</D:status></D:response>" + closing, contacts));
    assert(!dtmail_carddav_read("<!DOCTYPE multistatus [<!ENTITY x SYSTEM 'file:///dev/null'>]>" +
        opening + "&x;" + closing, contacts));
    assert(!dtmail_carddav_read("<multistatus xmlns='https://example.test/'/>", contacts));
    assert(!dtmail_carddav_read(opening, contacts));
    assert(contacts.size() == 2);
    assert(dtmail_carddav_read(opening + closing, contacts) && contacts.empty());
}
