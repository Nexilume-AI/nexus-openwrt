#include "agent_mesh_profile.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define PIN "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

static bool parse(const char *paths) {
    char json[8192]; struct agent_mesh_profile profile;
    snprintf(json, sizeof(json), "{\"v\":2,\"seed_id\":\"" PIN "\",\"directory_sha256\":\"" PIN "\",\"relay_sha256\":\"" PIN "\",\"paths\":%s}", paths);
    return agent_mesh_profile_parse(json, &profile);
}
int main(void) {
    const char *good[] = {"192.168.250.1", "fd42::1", "2001:db8::1", "mesh.example.org", "seed-host"};
    const char *bad[] = {"", "localhost", "127.0.0.1", "::1", "::", "0.0.0.0", "fe80::1", "ff02::1", "224.0.0.1", "::ffff:192.168.1.1", "192.168.1.1/24", "https://seed", "seed:1234", "a..b", "a.-b", "a_b", "1.2.3", "0x7f000001"};
    for (size_t i=0; i<sizeof(good)/sizeof(*good); ++i) assert(agent_mesh_host_valid(good[i]));
    for (size_t i=0; i<sizeof(bad)/sizeof(*bad); ++i) {
        /* Numeric libc aliases are rejected again after DNS resolution. */
        if (!strcmp(bad[i], "0x7f000001")) continue;
        assert(!agent_mesh_host_valid(bad[i]));
    }
    const char *path="{\"directory_host\":\"seed.example\",\"directory_port\":443,\"relay_host\":\"fd42::1\",\"relay_port\":17444}";
    char paths[4096]; snprintf(paths,sizeof(paths),"[%s]",path); assert(parse(paths));
    snprintf(paths,sizeof(paths),"[%s,%s]",path,path); assert(!parse(paths));
    strcpy(paths,"[");
    for (int i=0;i<5;++i) {
        char entry[256]; snprintf(entry,sizeof(entry),"%s{\"directory_host\":\"seed.example\",\"directory_port\":%d,\"relay_host\":\"fd42::1\",\"relay_port\":17444}",i?",":"",18443+i);
        strcat(paths,entry);
        if (i==3) { char four[4096]; snprintf(four,sizeof(four),"%s]",paths); assert(parse(four)); }
    }
    strcat(paths,"]"); assert(!parse(paths));
    assert(!parse("[]")); assert(!parse("{}")); assert(!parse("[null]"));
    assert(!parse("[{\"directory_host\":\"seed\",\"directory_port\":\"443\",\"relay_host\":\"seed\",\"relay_port\":443}]"));
    assert(!parse("[{\"directory_host\":\"seed\",\"directory_port\":0,\"relay_host\":\"seed\",\"relay_port\":443}]"));
    assert(!parse("[{\"directory_host\":\"seed\",\"directory_port\":443,\"relay_host\":\"seed\",\"relay_port\":65536}]"));
    struct agent_mesh_profile profile; assert(!agent_mesh_profile_parse("{}",&profile));
    struct sockaddr_storage address; socklen_t length;
    assert(agent_mesh_resolve("192.168.250.1",443,0,&address,&length) && address.ss_family==AF_INET);
    assert(agent_mesh_resolve("fd42::1",443,0,&address,&length) && address.ss_family==AF_INET6);
    assert(!agent_mesh_resolve("0x7f000001",443,0,&address,&length));
    mbedtls_x509_crt cert={0}; cert.raw.p=(unsigned char *)"abc"; cert.raw.len=3;
    char hash[]="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    assert(agent_mesh_certificate_matches(&cert,hash));
    for (size_t i=0;i<64;++i) { char old=hash[i]; hash[i]=old=='0'?'1':'0'; assert(!agent_mesh_certificate_matches(&cert,hash)); hash[i]=old; }
    assert(!agent_mesh_certificate_matches(NULL,hash));
    puts("MESH_PROFILE_NATIVE_PASS"); return 0;
}
