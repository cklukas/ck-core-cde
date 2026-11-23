#ifndef DTMAIL_CARDDAV_CONTACTS_H
#define DTMAIL_CARDDAV_CONTACTS_H

#include "AddressAutocomplete.h"

#include <string>
#include <vector>

// Parse one successful DAV collection response without changing the caller's
// snapshot on error.
bool dtmail_carddav_read(const std::string &xml, std::vector<DtMailContact> &contacts);

// Schedule a refresh for the given collection. Results remain available while
// the same collection is offline. An empty collection clears the snapshot.
void dtmail_carddav_refresh(const char *url, const char *username, const char *ca_file);
int dtmail_carddav_match(const char *query, char results[][256], int capacity);

#endif
