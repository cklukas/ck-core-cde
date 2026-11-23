#ifndef DTMAIL_MAIL_SIGNATURE_H
#define DTMAIL_MAIL_SIGNATURE_H

#include <cstddef>

enum class DtMailSignatureStatus {
    Unsigned,
    Trusted,
    Untrusted,
    Invalid,
    MissingKey,
    Unverifiable
};

enum class DtMailSignatureKind { NoSignature, OpenPGP, SMime, Unknown };

struct DtMailSignature {
    DtMailSignatureKind kind;
    DtMailSignatureStatus status;
};

constexpr size_t dtmail_signature_limit = 32 * 1024 * 1024;
void dtmail_init_mime();
DtMailSignature dtmail_verify_signature(const char *message, size_t length);

#endif
