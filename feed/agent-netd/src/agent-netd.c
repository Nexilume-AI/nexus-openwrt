#include "agent_public_ipv6.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <ifaddrs.h>
#include <libubox/blobmsg.h>
#include <libubox/uloop.h>
#include <libubus.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/neighbour.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NETD_INTERFACE_LEN 32U
#define NETD_DEFAULT_INTERFACE "nexus-agent0"
#define NETD_DEFAULT_MAX_ADDRESSES 256U

static struct ubus_context *ubus_ctx;
static struct blob_buf response;
static struct agent_public_ipv6_pool pool;
static char interface_name[NETD_INTERFACE_LEN] = NETD_DEFAULT_INTERFACE;
static unsigned int interface_index;
static char upstream_interface[IFNAMSIZ];
static unsigned int upstream_interface_index;
static bool upstream_ndp_relay;
static struct uloop_timeout prune_timer;

enum address_field {
    ADDRESS_ROUTE_ID,
    ADDRESS_VALUE,
    ADDRESS_ORIGIN,
    ADDRESS_EXPIRES_MS,
    __ADDRESS_MAX
};

static const struct blobmsg_policy address_policy[__ADDRESS_MAX] = {
    [ADDRESS_ROUTE_ID] = {.name = "route_id", .type = BLOBMSG_TYPE_STRING},
    [ADDRESS_VALUE] = {.name = "address", .type = BLOBMSG_TYPE_STRING},
    [ADDRESS_ORIGIN] = {.name = "origin", .type = BLOBMSG_TYPE_STRING},
    [ADDRESS_EXPIRES_MS] = {.name = "expires_ms", .type = BLOBMSG_TYPE_INT64}
};

static uint64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0U;
    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

static bool valid_interface(const char *value)
{
    size_t index;
    if (value == NULL || strncmp(value, "nexus-agent", 11U) != 0 ||
        strlen(value) >= NETD_INTERFACE_LEN) return false;
    for (index = 0U; value[index] != '\0'; index++) {
        char c = value[index];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool valid_upstream_interface(const char *value)
{
    size_t index;
    size_t length;

    if (value == NULL) return false;
    length = strlen(value);
    if (length == 0U || length >= IFNAMSIZ ||
        strncmp(value, "nexus-agent", 11U) == 0) return false;
    for (index = 0U; index < length; index++) {
        char c = value[index];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.'))
            return false;
    }
    return true;
}

static bool parse_size(const char *text, size_t *value)
{
    char *end = NULL;
    unsigned long parsed;
    if (text == NULL || value == NULL) return false;
    parsed = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0UL || parsed > 65535UL)
        return false;
    *value = (size_t)parsed;
    return true;
}

static int netlink_address(const char *address, bool add)
{
    struct {
        struct nlmsghdr header;
        struct ifaddrmsg message;
        char attributes[RTA_SPACE(sizeof(struct in6_addr))];
    } request;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct in6_addr parsed;
    struct rtattr *attribute;
    struct {
        struct nlmsghdr header;
        struct nlmsgerr error;
    } reply;
    ssize_t received;
    int fd;

    if (inet_pton(AF_INET6, address, &parsed) != 1) return EINVAL;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return errno;
    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct ifaddrmsg));
    request.header.nlmsg_type = add ? RTM_NEWADDR : RTM_DELADDR;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (add) request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
    request.header.nlmsg_seq = 1U;
    request.message.ifa_family = AF_INET6;
    request.message.ifa_prefixlen = 128U;
    request.message.ifa_scope = RT_SCOPE_UNIVERSE;
    request.message.ifa_index = interface_index;
    attribute = (struct rtattr *)(((char *)&request) +
                                  NLMSG_ALIGN(request.header.nlmsg_len));
    attribute->rta_type = IFA_LOCAL;
    attribute->rta_len = RTA_LENGTH(sizeof(parsed));
    memcpy(RTA_DATA(attribute), &parsed, sizeof(parsed));
    request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) +
                               RTA_LENGTH(sizeof(parsed));
    if (sendto(fd, &request, request.header.nlmsg_len, 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        int status = errno;
        close(fd);
        return status;
    }
    received = recv(fd, &reply, sizeof(reply), 0);
    close(fd);
    if (received < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
        reply.header.nlmsg_type != NLMSG_ERROR) return EPROTO;
    return reply.error.error == 0 ? 0 : -reply.error.error;
}

static int netlink_proxy_neighbor(const char *address, bool add)
{
    struct {
        struct nlmsghdr header;
        struct ndmsg message;
        char attributes[RTA_SPACE(sizeof(struct in6_addr))];
    } request;
    struct sockaddr_nl kernel = {.nl_family = AF_NETLINK};
    struct in6_addr parsed;
    struct rtattr *attribute;
    struct {
        struct nlmsghdr header;
        struct nlmsgerr error;
    } reply;
    ssize_t received;
    int fd;

    if (!upstream_ndp_relay) return 0;
    if (inet_pton(AF_INET6, address, &parsed) != 1) return EINVAL;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return errno;
    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len = NLMSG_LENGTH(sizeof(struct ndmsg));
    request.header.nlmsg_type = add ? RTM_NEWNEIGH : RTM_DELNEIGH;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    if (add) request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    request.header.nlmsg_seq = 1U;
    request.message.ndm_family = AF_INET6;
    request.message.ndm_ifindex = (int)upstream_interface_index;
    request.message.ndm_state = add ? NUD_PERMANENT : NUD_NONE;
    request.message.ndm_flags = NTF_PROXY;
    attribute = (struct rtattr *)(((char *)&request) +
                                  NLMSG_ALIGN(request.header.nlmsg_len));
    attribute->rta_type = NDA_DST;
    attribute->rta_len = RTA_LENGTH(sizeof(parsed));
    memcpy(RTA_DATA(attribute), &parsed, sizeof(parsed));
    request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) +
                               RTA_LENGTH(sizeof(parsed));
    if (sendto(fd, &request, request.header.nlmsg_len, 0,
               (struct sockaddr *)&kernel, sizeof(kernel)) < 0) {
        int status = errno;
        close(fd);
        return status;
    }
    received = recv(fd, &reply, sizeof(reply), 0);
    close(fd);
    if (received < (ssize_t)NLMSG_LENGTH(sizeof(struct nlmsgerr)) ||
        reply.header.nlmsg_type != NLMSG_ERROR) return EPROTO;
    return reply.error.error == 0 ? 0 : -reply.error.error;
}

static bool enable_proxy_ndp(void)
{
    char path[128];
    int fd;
    int written;

    if (!upstream_ndp_relay) return true;
    written = snprintf(path, sizeof(path),
        "/proc/sys/net/ipv6/conf/%s/proxy_ndp", upstream_interface);
    if (written <= 0 || (size_t)written >= sizeof(path)) return false;
    /* Docker may configure this per-interface sysctl before starting us and
     * expose /proc/sys read-only. Do not require a redundant privileged write. */
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        char value[8] = {0};
        ssize_t size = read(fd, value, sizeof(value) - 1U);
        close(fd);
        if (size > 0 && (strcmp(value, "1\n") == 0 || strcmp(value, "1") == 0))
            return true;
    }
    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    written = (int)write(fd, "1\n", 2U);
    close(fd);
    return written == 2;
}

static void flush_managed_addresses(void)
{
    struct ifaddrs *addresses = NULL;
    struct ifaddrs *current;

    if (getifaddrs(&addresses) != 0) return;
    for (current = addresses; current != NULL; current = current->ifa_next) {
        char text[AGENT_PUBLIC_IPV6_TEXT_LEN];
        struct sockaddr_in6 *ipv6;
        if (current->ifa_addr == NULL ||
            current->ifa_addr->sa_family != AF_INET6 ||
            strcmp(current->ifa_name, interface_name) != 0) continue;
        ipv6 = (struct sockaddr_in6 *)current->ifa_addr;
        if (inet_ntop(AF_INET6, &ipv6->sin6_addr, text, sizeof(text)) == NULL ||
            !agent_public_ipv6_contains(&pool, text)) continue;
        (void)netlink_address(text, false);
    }
    freeifaddrs(addresses);
}

static void flush_active_proxy_neighbors(void)
{
    size_t index;

    if (!upstream_ndp_relay) return;
    for (index = 0U; index < pool.count; index++)
        (void)netlink_proxy_neighbor(pool.leases[index].address, false);
}

static int parse_request(
    struct blob_attr *message,
    const char **route_id,
    const char **address,
    const char **origin,
    uint64_t *expires_ms,
    bool require_expiry
)
{
    struct blob_attr *attributes[__ADDRESS_MAX] = {0};
    blobmsg_parse(address_policy, __ADDRESS_MAX, attributes,
                  blobmsg_data(message), blobmsg_len(message));
    if (attributes[ADDRESS_ROUTE_ID] == NULL ||
        attributes[ADDRESS_VALUE] == NULL) return UBUS_STATUS_INVALID_ARGUMENT;
    *route_id = blobmsg_get_string(attributes[ADDRESS_ROUTE_ID]);
    *address = blobmsg_get_string(attributes[ADDRESS_VALUE]);
    if (require_expiry) {
        if (attributes[ADDRESS_ORIGIN] == NULL ||
            attributes[ADDRESS_EXPIRES_MS] == NULL)
            return UBUS_STATUS_INVALID_ARGUMENT;
        *origin = blobmsg_get_string(attributes[ADDRESS_ORIGIN]);
        if ((*origin)[0] == '\0') return UBUS_STATUS_INVALID_ARGUMENT;
        *expires_ms = blobmsg_get_u64(attributes[ADDRESS_EXPIRES_MS]);
        if (*expires_ms <= monotonic_ms()) return UBUS_STATUS_INVALID_ARGUMENT;
    }
    return UBUS_STATUS_OK;
}

static int allocate_address(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    const char *route_id;
    const char *address;
    const char *origin = NULL;
    struct agent_public_ipv6_lease lease;
    enum agent_public_ipv6_result result;
    bool existed;
    uint64_t expires_ms = 0U;
    int status;

    (void)object;
    (void)method;
    status = parse_request(message, &route_id, &address, &origin,
                           &expires_ms, true);
    if (status != UBUS_STATUS_OK) return status;
    existed = agent_public_ipv6_find(&pool, route_id) != NULL;
    result = agent_public_ipv6_allocate(
        &pool, route_id, origin, expires_ms, &lease);
    if (result != AGENT_PUBLIC_IPV6_OK || strcmp(lease.address, address) != 0) {
        if (!existed && result == AGENT_PUBLIC_IPV6_OK)
            (void)agent_public_ipv6_release(&pool, route_id, NULL);
        return result == AGENT_PUBLIC_IPV6_FULL ?
            UBUS_STATUS_NOT_SUPPORTED : UBUS_STATUS_PERMISSION_DENIED;
    }
    if (!existed) {
        status = netlink_address(address, true);
        if (status != 0) {
            (void)agent_public_ipv6_release(&pool, route_id, NULL);
            return UBUS_STATUS_UNKNOWN_ERROR;
        }
    }
    status = netlink_proxy_neighbor(address, true);
    if (status != 0) {
        if (!existed) {
            (void)netlink_address(address, false);
            (void)agent_public_ipv6_release(&pool, route_id, NULL);
        }
        return UBUS_STATUS_UNKNOWN_ERROR;
    }
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "applied", true);
    blobmsg_add_string(&response, "interface", interface_name);
    blobmsg_add_string(&response, "address_source",
        upstream_ndp_relay ? "upstream-relay" : "routed-prefix");
    blobmsg_add_string(&response, "address", lease.address);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int release_address(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    const char *route_id;
    const char *address;
    const char *ignored_origin = NULL;
    const struct agent_public_ipv6_lease *lease;
    int status;
    uint64_t ignored_expiry = 0U;

    (void)object;
    (void)method;
    status = parse_request(message, &route_id, &address, &ignored_origin,
                           &ignored_expiry, false);
    if (status != UBUS_STATUS_OK) return status;
    lease = agent_public_ipv6_find(&pool, route_id);
    if (lease == NULL) return UBUS_STATUS_NOT_FOUND;
    if (strcmp(lease->address, address) != 0)
        return UBUS_STATUS_PERMISSION_DENIED;
    status = netlink_proxy_neighbor(address, false);
    if (status != 0 && status != ENOENT) return UBUS_STATUS_UNKNOWN_ERROR;
    status = netlink_address(address, false);
    if (status != 0 && status != EADDRNOTAVAIL) return UBUS_STATUS_UNKNOWN_ERROR;
    (void)agent_public_ipv6_release(&pool, route_id, NULL);
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "released", true);
    blobmsg_add_string(&response, "address", address);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static int netd_status(
    struct ubus_context *ctx,
    struct ubus_object *object,
    struct ubus_request_data *request,
    const char *method,
    struct blob_attr *message
)
{
    (void)object;
    (void)method;
    (void)message;
    blob_buf_init(&response, 0);
    blobmsg_add_u8(&response, "ready", true);
    blobmsg_add_string(&response, "interface", interface_name);
    blobmsg_add_u32(&response, "ifindex", interface_index);
    blobmsg_add_string(&response, "prefix", pool.prefix_text);
    blobmsg_add_string(&response, "address_source",
        upstream_ndp_relay ? "upstream-relay" : "routed-prefix");
    blobmsg_add_string(&response, "upstream_interface",
        upstream_ndp_relay ? upstream_interface : "");
    blobmsg_add_u32(&response, "active", (uint32_t)pool.count);
    blobmsg_add_u32(&response, "capacity", (uint32_t)pool.capacity);
    ubus_send_reply(ctx, request, response.head);
    return UBUS_STATUS_OK;
}

static bool expire_address(
    const struct agent_public_ipv6_lease *lease,
    void *context
)
{
    int status;
    (void)context;
    status = netlink_proxy_neighbor(lease->address, false);
    if (status != 0 && status != ENOENT) return false;
    status = netlink_address(lease->address, false);
    return status == 0 || status == EADDRNOTAVAIL;
}

static void prune_addresses(struct uloop_timeout *timeout)
{
    (void)timeout;
    (void)agent_public_ipv6_prune(
        &pool, monotonic_ms(), expire_address, NULL);
    uloop_timeout_set(&prune_timer, 1000);
}

static const struct ubus_method methods[] = {
    UBUS_METHOD("allocate", allocate_address, address_policy),
    UBUS_METHOD("renew", allocate_address, address_policy),
    UBUS_METHOD("release", release_address, address_policy),
    UBUS_METHOD_NOARG("status", netd_status)
};

static struct ubus_object_type object_type =
    UBUS_OBJECT_TYPE("agent.netd", methods);
static struct ubus_object object = {
    .name = "agent.netd",
    .type = &object_type,
    .methods = methods,
    .n_methods = ARRAY_SIZE(methods)
};

static void stop_signal(int signal_number)
{
    (void)signal_number;
    uloop_end();
}

int main(int argc, char **argv)
{
    const char *prefix = NULL;
    size_t capacity = NETD_DEFAULT_MAX_ADDRESSES;
    int option;
    int status;

    while ((option = getopt(argc, argv, "i:p:m:r:h")) != -1) {
        if (option == 'i') {
            if (!valid_interface(optarg)) return EXIT_FAILURE;
            snprintf(interface_name, sizeof(interface_name), "%s", optarg);
        } else if (option == 'p') {
            prefix = optarg;
        } else if (option == 'm') {
            if (!parse_size(optarg, &capacity)) return EXIT_FAILURE;
        } else if (option == 'r') {
            if (!valid_upstream_interface(optarg)) return EXIT_FAILURE;
            snprintf(upstream_interface, sizeof(upstream_interface), "%s",
                     optarg);
            upstream_ndp_relay = true;
        } else {
            fprintf(stderr, "Usage: %s -p prefix [-i nexus-interface] "
                            "[-m max_addresses] [-r upstream-interface]\n",
                            argv[0]);
            return option == 'h' ? EXIT_SUCCESS : EXIT_FAILURE;
        }
    }
    if (prefix == NULL || !agent_public_ipv6_pool_init(&pool, prefix, capacity))
        return EXIT_FAILURE;
    interface_index = if_nametoindex(interface_name);
    if (interface_index == 0U) {
        fprintf(stderr, "agent-netd: dedicated interface does not exist\n");
        agent_public_ipv6_pool_free(&pool);
        return EXIT_FAILURE;
    }
    if (upstream_ndp_relay) {
        upstream_interface_index = if_nametoindex(upstream_interface);
        if (upstream_interface_index == 0U || !enable_proxy_ndp()) {
            fprintf(stderr, "agent-netd: upstream NDP relay is unavailable\n");
            agent_public_ipv6_pool_free(&pool);
            return EXIT_FAILURE;
        }
    }
    /* procd respawn does not rerun the init-script preamble. Reconcile only
     * addresses in the configured prefix on the dedicated interface here. */
    flush_managed_addresses();
    signal(SIGINT, stop_signal);
    signal(SIGTERM, stop_signal);
    uloop_init();
    ubus_ctx = ubus_connect(NULL);
    if (ubus_ctx == NULL) return EXIT_FAILURE;
    ubus_add_uloop(ubus_ctx);
    status = ubus_add_object(ubus_ctx, &object);
    if (status != UBUS_STATUS_OK) {
        ubus_free(ubus_ctx);
        return EXIT_FAILURE;
    }
    prune_timer.cb = prune_addresses;
    uloop_timeout_set(&prune_timer, 1000);
    uloop_run();
    uloop_timeout_cancel(&prune_timer);
    flush_active_proxy_neighbors();
    flush_managed_addresses();
    blob_buf_free(&response);
    ubus_free(ubus_ctx);
    uloop_done();
    agent_public_ipv6_pool_free(&pool);
    return EXIT_SUCCESS;
}
