/* Deliberately unavailable UCI for the host-side pause regression. This stub
 * is private to test_static_peer_pause; firmware always links real libuci. */
#ifndef NEXUS_TEST_STATIC_PEERS_UCI_H
#define NEXUS_TEST_STATIC_PEERS_UCI_H
#include <stddef.h>
#define UCI_OK 0
struct uci_context { int unused; };
struct uci_element { const char *name; };
struct uci_section { struct uci_element e; const char *type; };
struct uci_package { int sections; };
extern unsigned int test_uci_accesses;
static inline struct uci_context *uci_alloc_context(void) {
    static struct uci_context context;
    test_uci_accesses++;
    return &context;
}
static inline void uci_free_context(struct uci_context *context) { (void)context; }
static inline int uci_load(struct uci_context *context, const char *name, struct uci_package **package) {
    (void)context; (void)name; (void)package; return -1;
}
static inline void uci_unload(struct uci_context *context, struct uci_package *package) {
    (void)context; (void)package;
}
static inline const char *uci_lookup_option_string(struct uci_context *context, struct uci_section *section, const char *name) {
    (void)context; (void)section; (void)name; return NULL;
}
#define uci_foreach_element(list, element) for ((void)(list), (element) = NULL; (element) != NULL; (element) = NULL)
#define uci_to_section(element) ((struct uci_section *)(element))
#endif
