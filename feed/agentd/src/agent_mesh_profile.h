#ifndef NEXUS_AGENT_MESH_PROFILE_H
#define NEXUS_AGENT_MESH_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <mbedtls/x509_crt.h>

#define AGENT_MESH_PROFILE_MAX 4096U
#define AGENT_MESH_PATHS_MAX 4U
#define AGENT_MESH_HOST_LEN 128U
struct agent_mesh_path {
    char directory_host[AGENT_MESH_HOST_LEN];
    char relay_host[AGENT_MESH_HOST_LEN];
    uint16_t directory_port;
    uint16_t relay_port;
};
struct agent_mesh_profile {
    char seed_id[65];
    char directory_sha256[65];
    char relay_sha256[65];
    size_t count;
    struct agent_mesh_path paths[AGENT_MESH_PATHS_MAX];
};
bool agent_mesh_profile_parse(const char *json, struct agent_mesh_profile *profile);
bool agent_mesh_host_valid(const char *host);
bool agent_mesh_resolve(const char *host, uint16_t port, uint64_t attempt,
                        struct sockaddr_storage *address, socklen_t *length);
bool agent_mesh_certificate_matches(const mbedtls_x509_crt *certificate,
                                    const char *fingerprint);
#endif
