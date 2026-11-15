/*
 * Behavioural check of the XDMCP ForwardQuery handler (xdmcp.c, forward_respond).
 *
 * The runner extracts forward_respond verbatim from xdmcp.c into a generated
 * header (FORWARD_RESPOND_H) and compiles it here against stand-ins for the
 * packet reader and the reply path.  The client port of a request is an
 * ARRAY8 whose length the sender chooses; the handler used to fold every byte
 * of it into a signed int for its debug message, which overflows for a port of
 * four or more bytes.  Built with UBSan (no recovery) that is a failure.
 *
 * Scenarios: a valid 2-byte port gives the same debug text and the same reply
 * address as before; ports of 0, 1, 3, 8 and 65535 bytes are survived, are
 * not answered (an Internet port is 16 bits) and report a port computed from
 * their first two bytes only.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef unsigned char CARD8, *CARD8Ptr;
typedef unsigned short CARD16;
typedef struct { CARD16 length; CARD8Ptr data; } ARRAY8, *ARRAY8Ptr;
typedef struct { CARD8 length; ARRAY8Ptr data; } ARRAYofARRAY8, *ARRAYofARRAY8Ptr;
typedef struct { unsigned char *data; int size, pointer, count; } XdmcpBuffer, *XdmcpBufferPtr;
typedef int xdmOpCode;
#define FORWARD_QUERY 3

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

static char debug_text[256 * 1024];
void Debug(char *fmt, ...)
{
    va_list args;
    size_t used = strlen(debug_text);
    va_start(args, fmt);
    vsnprintf(debug_text + used, sizeof debug_text - used, fmt, args);
    va_end(args);
}

/* The wire format is libXdmcp's: big-endian 16 bit length, then the bytes. */
static int ReadCard16(XdmcpBufferPtr b, CARD16 *value)
{
    if (b->pointer + 2 > b->count) return 0;
    *value = (CARD16)(b->data[b->pointer] << 8 | b->data[b->pointer + 1]);
    b->pointer += 2;
    return 1;
}
int XdmcpReadARRAY8(XdmcpBufferPtr b, ARRAY8Ptr a)
{
    CARD16 length;
    if (!ReadCard16(b, &length) || b->pointer + length > b->count) return 0;
    a->length = length;
    a->data = malloc(length ? length : 1);
    memcpy(a->data, b->data + b->pointer, length);
    b->pointer += length;
    return 1;
}
int XdmcpReadARRAYofARRAY8(XdmcpBufferPtr b, ARRAYofARRAY8Ptr a)
{
    if (b->pointer + 1 > b->count) return 0;
    a->length = b->data[b->pointer++];
    a->data = calloc(a->length ? a->length : 1, sizeof *a->data);
    for (int i = 0; i < a->length; i++)
        if (!XdmcpReadARRAY8(b, &a->data[i])) return 0;
    return 1;
}
void XdmcpDisposeARRAY8(ARRAY8Ptr a) { free(a->data); a->data = nullptr; a->length = 0; }
void XdmcpDisposeARRAYofARRAY8(ARRAYofARRAY8Ptr a)
{
    for (int i = 0; i < a->length; i++) XdmcpDisposeARRAY8(&a->data[i]);
    free(a->data); a->data = nullptr; a->length = 0;
}

static int replies;
static struct sockaddr_in reply_to;
static int reply_type;
static void all_query_respond(struct sockaddr *from, int fromlen,
                              ARRAYofARRAY8Ptr authenticationNames, xdmOpCode type)
{
    (void)authenticationNames;
    CHECK(fromlen == (int)sizeof reply_to);
    memcpy(&reply_to, from, sizeof reply_to);
    reply_type = type;
    ++replies;
}

static XdmcpBuffer buffer;

#include FORWARD_RESPOND_H

/* Run forward_respond on a request naming ADDRESS (4 bytes) and a port array
 * of PORT_LENGTH bytes taken from PORT (repeated or 0xff-filled as needed). */
static void forward(const unsigned char *port, size_t port_length)
{
    static const unsigned char address[4] = {10, 1, 2, 3};
    size_t total = 2 + 4 + 2 + port_length + 1;
    unsigned char *wire = calloc(1, total), *p = wire;
    struct sockaddr_in from = { .sin_family = AF_INET };

    *p++ = 0; *p++ = 4; memcpy(p, address, 4); p += 4;
    *p++ = (unsigned char)(port_length >> 8); *p++ = (unsigned char)port_length;
    memcpy(p, port, port_length); p += port_length;
    *p++ = 0;                                            /* no authentication names */
    buffer.data = wire; buffer.size = buffer.count = (int)total; buffer.pointer = 0;
    debug_text[0] = '\0'; replies = 0;
    forward_respond((struct sockaddr *)&from, sizeof from, (int)total);
    free(wire);
}

int main(void)
{
    unsigned char port[65535];

    /* A valid port 6001 (0x1771): reply goes to 10.1.2.3 port 6001. */
    forward((const unsigned char[]){0x17, 0x71}, 2);
    CHECK(strstr(debug_text, "Forward client address (port 6001) 10 1 2 3\n"));
    CHECK(replies == 1 && reply_type == FORWARD_QUERY);
    CHECK(reply_to.sin_family == AF_INET && ntohs(reply_to.sin_port) == 6001);
    CHECK(memcmp(&reply_to.sin_addr, (unsigned char[]){10, 1, 2, 3}, 4) == 0);

    /* The highest and the lowest 16 bit ports. */
    forward((const unsigned char[]){0xff, 0xff}, 2);
    CHECK(strstr(debug_text, "(port 65535)") && replies == 1 && ntohs(reply_to.sin_port) == 65535);
    forward((const unsigned char[]){0x00, 0x00}, 2);
    CHECK(strstr(debug_text, "(port 0)") && replies == 1);

    /* Ports that are not 16 bits wide are not answered. */
    forward(port, 0);
    CHECK(strstr(debug_text, "(port 0)") && replies == 0);
    forward((const unsigned char[]){0x2a}, 1);
    CHECK(strstr(debug_text, "(port 42)") && replies == 0);
    forward((const unsigned char[]){0xff, 0xff, 0xff}, 3);
    CHECK(strstr(debug_text, "(port 65535)") && replies == 0);

    /* Long arrays of 0xff overflowed a signed int after four bytes. */
    memset(port, 0xff, sizeof port);
    forward(port, 8);
    CHECK(strstr(debug_text, "(port 65535)") && replies == 0);
    forward(port, 4);
    CHECK(strstr(debug_text, "(port 65535)") && replies == 0);
    forward(port, sizeof port);
    CHECK(strstr(debug_text, "(port 65535)") && replies == 0);

    puts("forward port: 9 scenarios passed");
    return 0;
}
