/*
 * Fuzz target: the request wire layer of dtspcd in libDtSvc (DtEncap/spc-proto.c).
 *
 * run-fuzz.py extracts the production functions into FUZZ_PRODUCTION_H and the
 * size constants and structures from the production headers into FUZZ_TYPES_H.
 * What is stubbed here is only the world around them: the socket (read() takes
 * its bytes from the fuzz input), the error log, the connection list and the
 * channel table.
 *
 * Input layout: byte 0 selects the entry point, the rest is the data.
 *   0  header decode        the first 20 bytes are the request header
 *   1  application data     payload of an APPLICATION request
 *   2  device data          payload of a QUERY_DEVICES reply
 *   3  logfile data         payload of a QUERY_LOGFILE request
 *   4  register data        payload of a REGISTER request
 *   5  termios request      payload of a CHANNEL_TERMIOS request
 *   6  whole request        header and payload read from the "socket", then
 *                           every payload decoder runs on the result
 *   7  multi-packet         a first request is read, then SPC_Get_Multi_Packet
 *                           reads the further packets from the "socket"
 */
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>

typedef char XeChar;
typedef XeChar *XeString;
#define TRUE 1
#define FALSE 0
#define ERROR (-1)
#define SPC_ERROR FALSE
#define Space ((XeChar)' ')
#define SPC_UNKNOWN_HOSTINFO_STR "HP-UX:7.0:*"
#define UNUSED_PARM(x) x __attribute__((unused))
#define XeString_NULL ((XeString)0)

typedef void *SPC_Channel_Ptr;
typedef struct { int sid; XeChar connected; } SPC_Connection, *SPC_Connection_Ptr;

#include FUZZ_TYPES_H

/* locking, allocation, error log, connection and channel table: stand-ins */
#define _DtSvcProcessLock() ((void)0)
#define _DtSvcProcessUnlock() ((void)0)
#define XeFree(p) free(p)
#define CONNECTION_HOSTNAME(conn) strdup("")
static void *XeMalloc(size_t n) { void *p = malloc(n); if (!p) abort(); return p; }
static protocol_request_ptr free_protocol_requests;
static void SPC_Error(int code, ...) { (void)code; }
static void SPC_Close_Connection(SPC_Connection_Ptr c) { c->connected = FALSE; }
static SPC_Channel_Ptr SPC_Lookup_Channel(int cid, SPC_Connection_Ptr c) { (void)cid; (void)c; return nullptr; }
static int print_protocol_request(XeString name, protocol_request_ptr p) { (void)name; (void)p; return 0; }
enum { SPC_Protocol = 1, SPC_Buffer_Overflow, SPC_Out_Of_Memory, SPC_Connection_Reset, SPC_Reading,
       SPC_Connection_EOF };
static protocol_request_ptr SPC_Filter_Connection(SPC_Connection_Ptr connection, SPC_Channel_Ptr channel,
                                                  int reqtype, int deletep);

/* The "socket": read() hands out the fuzz bytes. */
static const uint8_t *stream;
static size_t stream_left;
static ssize_t fuzz_read(int fd, void *buf, size_t n)
{
    (void)fd;
    if (n > stream_left) n = stream_left;
    memcpy(buf, stream, n);
    stream += n;
    stream_left -= n;
    return (ssize_t)n;
}
#define read(fd, buf, n) fuzz_read(fd, buf, n)

#include FUZZ_PRODUCTION_H

/* The production filter queues requests for other channels; with no channel table
   every request is for the caller, so the next request read is the answer. */
static protocol_request_ptr SPC_Filter_Connection(SPC_Connection_Ptr connection, SPC_Channel_Ptr channel,
                                                  int reqtype, int deletep)
{
    (void)channel; (void)reqtype; (void)deletep;
    return SPC_Read_Protocol(connection);
}

static void free_vector(XeString *v)
{
    if (!v) return;
    for (XeString *p = v; *p; p++) free(*p);
    free(v);
}

/* A request holding PAYLOAD, as SPC_Read_Protocol leaves it. */
static protocol_request_ptr make_request(const uint8_t *payload, size_t size)
{
    if (size > MAXPAYLOADLEN) size = MAXPAYLOADLEN;
    protocol_request_ptr prot = SPC_New_Protocol_Ptr(nullptr, 0, (int)size);
    memset(prot->dataptr->data, 0, sizeof prot->dataptr->data);
    memcpy(PDRP(prot->dataptr), payload, size);
    prot->dataptr->data[REQUEST_HEADER_LENGTH + size] = '\0';
    prot->dataptr->len = (int)size;
    prot->dataptr->offset = REQUEST_HEADER_LENGTH;
    return prot;
}

static void decode_payload(protocol_request_ptr prot, int mode)
{
    XeString a, b, c, d, e, f;
    XeString *argv, *envp;
    XeString buf = PDRP(prot->dataptr), end = PDEND(prot->dataptr);

    switch (mode) {
    case 1:
        if (sscan_application_data(buf, end, nullptr, &a, &b, &argv, &envp, 0) == TRUE) {
            free(a); free(b); free_vector(argv); free_vector(envp);
        }
        break;
    case 2:
        if (sscan_device_data(buf, end, &a, &b, &c, &d, &e, &f) == TRUE) {
            free(a); free(b); free(c); free(d); free(e); free(f);
        }
        break;
    case 3:
        if (sscan_logfile_data(buf, end, &a, &b, &c) == TRUE) {
            free(a); free(b); free(c);
        }
        break;
    case 4:
        if (sscan_register_data(buf, end, &a, &b, &c, &d) == TRUE) {
            free(a); free(b); free(c); free(d);
        }
        break;
    case 5: {
        int connector, side;
        char out[64];
        if (SPC_Read_Termios_Request(prot, &connector, &side, out, sizeof out))
            (void)strlen(out);
        break;
    }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0) return 0;
    int mode = data[0] % 8;
    data++; size--;

    if (mode == 0) {
        char header[REQUEST_HEADER_LENGTH + 1] = {0};
        int cid, type, len, seq;
        memcpy(header, data, size < REQUEST_HEADER_LENGTH ? size : REQUEST_HEADER_LENGTH);
        if (size >= REQUEST_HEADER_LENGTH)
            SPC_Parse_Header(header, &cid, &type, &len, &seq);
        return 0;
    }
    if (mode <= 5) {
        protocol_request_ptr prot = make_request(data, size);
        decode_payload(prot, mode);
        SPC_Free_Protocol_Ptr(prot);
        return 0;
    }

    SPC_Connection conn = { .sid = 3, .connected = TRUE };
    stream = data;
    stream_left = size;
    protocol_request_ptr prot = SPC_Read_Protocol(&conn);
    if (prot == SPC_ERROR) return 0;
    if (mode == 6) {
        for (int m = 1; m <= 5; m++)
            decode_payload(prot, m);
    } else {
        int outlen = 0;
        char **out = SPC_Get_Multi_Packet(&conn, prot, &outlen, prot->request_type, (XeString)"fuzz");
        if (out != SPC_ERROR) {
            for (int i = 0; i < outlen; i++) free(out[i]);
            free(out);
        }
    }
    SPC_Free_Protocol_Ptr(prot);
    return 0;
}
