#include "MailSignature.h"

#if DTMAIL_GMIME_ENABLED
#include <gmime/gmime.h>

#include <cstring>
#include <memory>

namespace {
struct Unref {
    void operator()(gpointer object) const { g_object_unref(object); }
};

template<class T> using GObjectPtr = std::unique_ptr<T, Unref>;

DtMailSignatureStatus summarize(GMimeSignatureList *signatures)
{
    using Status = DtMailSignatureStatus;
    if (!signatures || g_mime_signature_list_length(signatures) == 0)
        return Status::Unverifiable;

    Status outcome = Status::Trusted;
    for (int i = 0; i < g_mime_signature_list_length(signatures); ++i) {
        GMimeSignature *signature = g_mime_signature_list_get_signature(signatures, i);
        unsigned status = g_mime_signature_get_status(signature);
        if (status & GMIME_SIGNATURE_STATUS_KEY_MISSING) {
            outcome = Status::MissingKey;
            continue;
        }
        if (status & (GMIME_SIGNATURE_STATUS_RED | GMIME_SIGNATURE_STATUS_KEY_REVOKED))
            return Status::Invalid;
        if (status & GMIME_SIGNATURE_STATUS_SYS_ERROR) {
            if (outcome != Status::MissingKey)
                outcome = Status::Unverifiable;
            continue;
        }
        if ((status & GMIME_SIGNATURE_STATUS_ERROR_MASK) ||
            !(status & GMIME_SIGNATURE_STATUS_VALID)) {
            if (outcome == Status::Trusted)
                outcome = Status::Untrusted;
        }
    }
    return outcome;
}
}

void dtmail_init_mime()
{
    static const bool initialized = [] { g_mime_init(); return true; }();
    (void)initialized;
}

DtMailSignature dtmail_verify_signature(const char *raw, size_t length)
{
    using Kind = DtMailSignatureKind;
    using Status = DtMailSignatureStatus;
    if (!raw || !length || length > dtmail_signature_limit)
        return {Kind::Unknown, Status::Unverifiable};

    dtmail_init_mime();
    GObjectPtr<GMimeStream> stream(g_mime_stream_mem_new_with_buffer(raw, length));
    GObjectPtr<GMimeParser> parser(g_mime_parser_new_with_stream(stream.get()));
    if (length >= 5 && !memcmp(raw, "From ", 5))
        g_mime_parser_set_format(parser.get(), GMIME_FORMAT_MBOX);
    GObjectPtr<GMimeMessage> message(g_mime_parser_construct_message(parser.get(), nullptr));
    GMimeObject *body = message ? g_mime_message_get_mime_part(message.get()) : nullptr;
    if (!body)
        return {Kind::Unknown, Status::Unverifiable};

    if (GMIME_IS_MULTIPART_SIGNED(body)) {
        const char *protocol = g_mime_object_get_content_type_parameter(body, "protocol");
        Kind kind = Kind::Unknown;
        if (protocol && !g_ascii_strcasecmp(protocol, "application/pgp-signature"))
            kind = Kind::OpenPGP;
        else if (protocol &&
                 (!g_ascii_strcasecmp(protocol, "application/pkcs7-signature") ||
                  !g_ascii_strcasecmp(protocol, "application/x-pkcs7-signature")))
            kind = Kind::SMime;
        if (kind == Kind::Unknown || g_mime_multipart_get_count(GMIME_MULTIPART(body)) != 2)
            return {kind, Status::Unverifiable};

        GObjectPtr<GMimeSignatureList> signatures(g_mime_multipart_signed_verify(
            GMIME_MULTIPART_SIGNED(body), GMIME_VERIFY_NONE, nullptr));
        return {kind, summarize(signatures.get())};
    }

    if (GMIME_IS_APPLICATION_PKCS7_MIME(body)) {
        auto *cms = GMIME_APPLICATION_PKCS7_MIME(body);
        if (g_mime_application_pkcs7_mime_get_smime_type(cms) == GMIME_SECURE_MIME_TYPE_SIGNED_DATA) {
            GMimeObject *signed_content = nullptr;
            GObjectPtr<GMimeSignatureList> signatures(g_mime_application_pkcs7_mime_verify(
                cms, GMIME_VERIFY_NONE, &signed_content, nullptr));
            GObjectPtr<GMimeObject> content(signed_content);
            return {Kind::SMime, summarize(signatures.get())};
        }
    }
    return {Kind::NoSignature, Status::Unsigned};
}

#else
void dtmail_init_mime() {}
DtMailSignature dtmail_verify_signature(const char *, size_t)
{
    return {DtMailSignatureKind::Unknown, DtMailSignatureStatus::Unverifiable};
}
#endif
