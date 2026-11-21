#ifndef DTCM_SSH_TUNNEL_H
#define DTCM_SSH_TUNNEL_H

#include <stddef.h>

int _DtCm_ssh_tunnel_validate_host(const char *host, char *clean_out, size_t clean_sz);
int _DtCm_ssh_tunnel_requested(const char *host);
int _DtCm_ssh_tunnel_open(const char *host, char *local_sock_out, size_t sock_sz);
void _DtCm_ssh_tunnel_close_all(void);

#endif /* DTCM_SSH_TUNNEL_H */
