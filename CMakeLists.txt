cmake_minimum_required(VERSION 3.16)
project(nexus_openwrt_route_core LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# clock_gettime and strnlen are POSIX.1-2008 APIs in strict C17 builds.
if(UNIX)
    add_compile_definitions(_POSIX_C_SOURCE=200809L)
endif()

add_library(agent_route_core STATIC
    feed/agentd/src/route_table.c
    feed/agentd/src/agent_policy.c
    feed/agentd/src/agent_local_agents.c
)

add_library(agent_dynamic_manifest STATIC
    feed/agentd/src/agent_dynamic_manifest.c
)
target_include_directories(agent_dynamic_manifest PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)
target_link_libraries(agent_dynamic_manifest PUBLIC agent_adapter_contract)

add_library(agent_recovery STATIC
    feed/agentd/src/agent_recovery.c
)

add_library(agent_peer_core STATIC
    feed/agentd/src/peer_table.c
)

add_library(agent_peer_routes STATIC
    feed/agentd/src/agent_peer_routes.c
)

add_library(agent_discovery STATIC
    feed/agentd/src/agent_discovery.c
)

add_library(agent_cross_discovery STATIC
    feed/agentd/src/agent_cross_discovery.c
)

add_library(agent_discovery_promotion STATIC
    feed/agentd/src/agent_discovery_promotion.c
)

add_library(agent_auto_promotion STATIC
    feed/agentd/src/agent_auto_promotion.c
)

add_library(agent_card_authorization STATIC
    feed/agentd/src/agent_card_authorization.c
)

add_library(agent_card_directory_trust STATIC
    feed/agentd/src/agent_card_directory_trust.c
)

add_library(agent_relay_directory STATIC
    feed/agentd/src/agent_relay_directory.c
)

add_library(agent_relay_tunnel STATIC
    feed/common/agent_relay_tunnel.c
)

add_library(agent_arpx_protocol STATIC
    feed/common/agent_arpx_protocol.c
    feed/common/agent_peer_session.c
)

add_library(agent_peer_transport_contract STATIC
    feed/common/agent_peer_transport_contract.c
)

add_library(agent_ipc_protocol STATIC
    feed/common/agent_ipc_protocol.c
)

add_library(agent_public_ipv6 STATIC
    feed/common/agent_public_ipv6.c
)

add_library(agent_public_ingress STATIC
    feed/common/agent_public_ingress.c
)
target_include_directories(agent_public_ingress PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)
if(WIN32)
    target_link_libraries(agent_public_ingress PUBLIC ws2_32)
endif()

add_library(agent_edge_proxy STATIC
    feed/common/agent_edge_proxy.c
)
target_include_directories(agent_edge_proxy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)
target_link_libraries(agent_edge_proxy PUBLIC agent_public_ingress)

add_library(agent_rate_limiter STATIC
    feed/common/agent_rate_limiter.c
)

add_library(agent_tenant_quota STATIC
    feed/common/agent_tenant_quota.c
)

add_library(agent_gateway_contract STATIC
    feed/common/agent_gateway_contract.c
)

add_library(agent_auth_policy STATIC
    feed/common/agent_auth_policy.c
)

add_library(agent_replay_cache STATIC
    feed/common/agent_replay_cache.c
)

add_library(agent_stream_resume_cache STATIC
    feed/common/agent_stream_resume_cache.c
)

add_library(agent_invoke_contract STATIC
    feed/common/agent_invoke_contract.c
)

add_library(agent_adapter_contract STATIC
    feed/common/agent_adapter_contract.c
)

add_library(agent_adapter_ingress_contract STATIC
    feed/common/agent_adapter_ingress_contract.c
)

add_library(agent_sse_contract STATIC
    feed/common/agent_sse_contract.c
)

target_include_directories(agent_route_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
)

target_include_directories(agent_recovery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
)

target_include_directories(agent_peer_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_peer_routes PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_discovery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_cross_discovery PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_discovery_promotion PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_auto_promotion PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_card_authorization PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_card_directory_trust PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_relay_directory PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/agentd/src
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_relay_tunnel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_link_libraries(agent_discovery PUBLIC
    agent_peer_core agent_peer_transport_contract
)

target_link_libraries(agent_cross_discovery PUBLIC
    agent_peer_core
)

target_link_libraries(agent_discovery_promotion PUBLIC
    agent_discovery agent_cross_discovery agent_peer_core
)

target_link_libraries(agent_auto_promotion PUBLIC
    agent_discovery agent_peer_core
)

target_link_libraries(agent_card_authorization PUBLIC
    agent_cross_discovery agent_arpx_protocol
)

target_link_libraries(agent_card_directory_trust PUBLIC
    agent_card_authorization
)

target_link_libraries(agent_relay_directory PUBLIC
    agent_peer_core agent_peer_transport_contract
)

target_link_libraries(agent_peer_routes PUBLIC
    agent_route_core agent_peer_core agent_arpx_protocol
)

target_link_libraries(agent_peer_core PUBLIC
    agent_peer_transport_contract
)

target_include_directories(agent_arpx_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_peer_transport_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_link_libraries(agent_peer_transport_contract PUBLIC
    agent_arpx_protocol
)

target_include_directories(agent_ipc_protocol PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_rate_limiter PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_tenant_quota PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)
target_link_libraries(agent_tenant_quota PUBLIC agent_rate_limiter)

target_include_directories(agent_gateway_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_auth_policy PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_replay_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_stream_resume_cache PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_invoke_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_adapter_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_adapter_ingress_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

target_include_directories(agent_sse_contract PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)

if(MSVC)
    target_compile_options(agent_route_core PRIVATE /W4)
    target_compile_options(agent_recovery PRIVATE /W4)
    target_compile_options(agent_peer_core PRIVATE /W4)
    target_compile_options(agent_peer_routes PRIVATE /W4)
    target_compile_options(agent_discovery PRIVATE /W4)
    target_compile_options(agent_cross_discovery PRIVATE /W4)
    target_compile_options(agent_discovery_promotion PRIVATE /W4)
    target_compile_options(agent_auto_promotion PRIVATE /W4)
    target_compile_options(agent_card_authorization PRIVATE /W4)
    target_compile_options(agent_card_directory_trust PRIVATE /W4)
    target_compile_options(agent_relay_directory PRIVATE /W4)
    target_compile_options(agent_relay_tunnel PRIVATE /W4)
    target_compile_options(agent_arpx_protocol PRIVATE /W4)
    target_compile_options(agent_peer_transport_contract PRIVATE /W4)
    target_compile_options(agent_ipc_protocol PRIVATE /W4)
    target_compile_options(agent_rate_limiter PRIVATE /W4)
    target_compile_options(agent_tenant_quota PRIVATE /W4)
    target_compile_options(agent_gateway_contract PRIVATE /W4)
    target_compile_options(agent_auth_policy PRIVATE /W4)
    target_compile_options(agent_replay_cache PRIVATE /W4)
    target_compile_options(agent_stream_resume_cache PRIVATE /W4)
    target_compile_options(agent_invoke_contract PRIVATE /W4)
    target_compile_options(agent_adapter_contract PRIVATE /W4)
    target_compile_options(agent_adapter_ingress_contract PRIVATE /W4)
    target_compile_options(agent_sse_contract PRIVATE /W4)
else()
    target_compile_options(agent_route_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_recovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_peer_core PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_peer_routes PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_discovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_cross_discovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_discovery_promotion PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_auto_promotion PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_card_authorization PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_card_directory_trust PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_relay_directory PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_relay_tunnel PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_arpx_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_peer_transport_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_ipc_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_rate_limiter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_tenant_quota PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_gateway_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_auth_policy PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_replay_cache PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_stream_resume_cache PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_invoke_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_adapter_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_adapter_ingress_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
    target_compile_options(agent_sse_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

enable_testing()

add_executable(test_route_table tests/test_route_table.c)
target_link_libraries(test_route_table PRIVATE agent_route_core)

if(MSVC)
    target_compile_options(test_route_table PRIVATE /W4)
else()
    target_compile_options(test_route_table PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME route_table COMMAND test_route_table)

add_executable(test_agent_local_agents tests/test_agent_local_agents.c)
target_link_libraries(test_agent_local_agents PRIVATE agent_route_core)
if(MSVC)
    target_compile_options(test_agent_local_agents PRIVATE /W4)
else()
    target_compile_options(test_agent_local_agents PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME agent_local_agents COMMAND test_agent_local_agents)

add_executable(agent_route_benchmark
    feed/agentd/src/agent_route_benchmark.c
)
target_link_libraries(agent_route_benchmark PRIVATE agent_route_core)

if(MSVC)
    target_compile_options(agent_route_benchmark PRIVATE /W4 /O2)
else()
    target_compile_options(agent_route_benchmark PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2
    )
endif()

add_test(NAME route_table_capacity COMMAND agent_route_benchmark
    --routes 10000 --lookups 50000
)
set_tests_properties(route_table_capacity PROPERTIES LABELS "benchmark;p7.2")

add_executable(test_agent_recovery tests/test_agent_recovery.c)
target_link_libraries(test_agent_recovery PRIVATE agent_recovery)

if(MSVC)
    target_compile_options(test_agent_recovery PRIVATE /W4)
else()
    target_compile_options(test_agent_recovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_recovery COMMAND test_agent_recovery)

add_executable(test_agent_policy tests/test_agent_policy.c)
target_link_libraries(test_agent_policy PRIVATE agent_route_core)

if(MSVC)
    target_compile_options(test_agent_policy PRIVATE /W4)
else()
    target_compile_options(test_agent_policy PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_policy COMMAND test_agent_policy)

add_executable(test_peer_table tests/test_peer_table.c)
target_link_libraries(test_peer_table PRIVATE agent_peer_core)

if(MSVC)
    target_compile_options(test_peer_table PRIVATE /W4)
else()
    target_compile_options(test_peer_table PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME peer_table COMMAND test_peer_table)

add_executable(test_peer_routes tests/test_peer_routes.c)
target_link_libraries(test_peer_routes PRIVATE agent_peer_routes)

if(MSVC)
    target_compile_options(test_peer_routes PRIVATE /W4)
else()
    target_compile_options(test_peer_routes PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME peer_routes COMMAND test_peer_routes)

add_executable(test_agent_discovery tests/test_agent_discovery.c)
target_link_libraries(test_agent_discovery PRIVATE agent_discovery)

if(MSVC)
    target_compile_options(test_agent_discovery PRIVATE /W4)
else()
    target_compile_options(test_agent_discovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_discovery COMMAND test_agent_discovery)

add_executable(test_agent_cross_discovery tests/test_agent_cross_discovery.c)
target_link_libraries(test_agent_cross_discovery PRIVATE agent_cross_discovery)

if(MSVC)
    target_compile_options(test_agent_cross_discovery PRIVATE /W4)
else()
    target_compile_options(test_agent_cross_discovery PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_cross_discovery COMMAND test_agent_cross_discovery)

add_executable(test_agent_discovery_promotion
    tests/test_agent_discovery_promotion.c
)
target_link_libraries(test_agent_discovery_promotion PRIVATE
    agent_discovery_promotion
)

if(MSVC)
    target_compile_options(test_agent_discovery_promotion PRIVATE /W4)
else()
    target_compile_options(test_agent_discovery_promotion PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_discovery_promotion COMMAND test_agent_discovery_promotion)

add_executable(test_agent_auto_promotion
    tests/test_agent_auto_promotion.c
)
target_link_libraries(test_agent_auto_promotion PRIVATE agent_auto_promotion)

if(MSVC)
    target_compile_options(test_agent_auto_promotion PRIVATE /W4)
else()
    target_compile_options(test_agent_auto_promotion PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_auto_promotion COMMAND test_agent_auto_promotion)

add_executable(test_agent_card_authorization
    tests/test_agent_card_authorization.c
)
target_link_libraries(test_agent_card_authorization PRIVATE
    agent_card_authorization
)

if(MSVC)
    target_compile_options(test_agent_card_authorization PRIVATE /W4)
else()
    target_compile_options(test_agent_card_authorization PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_card_authorization COMMAND test_agent_card_authorization)

add_executable(test_agent_card_directory_trust
    tests/test_agent_card_directory_trust.c
)
target_link_libraries(test_agent_card_directory_trust PRIVATE
    agent_card_directory_trust
)

if(MSVC)
    target_compile_options(test_agent_card_directory_trust PRIVATE /W4)
else()
    target_compile_options(test_agent_card_directory_trust PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_card_directory_trust
    COMMAND test_agent_card_directory_trust)

add_executable(test_agent_relay_directory
    tests/test_agent_relay_directory.c
)
target_link_libraries(test_agent_relay_directory PRIVATE
    agent_relay_directory
)

if(MSVC)
    target_compile_options(test_agent_relay_directory PRIVATE /W4)
else()
    target_compile_options(test_agent_relay_directory PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_relay_directory COMMAND test_agent_relay_directory)

add_executable(test_agent_relay_tunnel
    tests/test_agent_relay_tunnel.c
)
target_link_libraries(test_agent_relay_tunnel PRIVATE agent_relay_tunnel)

if(MSVC)
    target_compile_options(test_agent_relay_tunnel PRIVATE /W4)
else()
    target_compile_options(test_agent_relay_tunnel PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME agent_relay_tunnel COMMAND test_agent_relay_tunnel)

add_executable(test_arpx_protocol tests/test_arpx_protocol.c)
target_link_libraries(test_arpx_protocol PRIVATE agent_arpx_protocol)

if(MSVC)
    target_compile_options(test_arpx_protocol PRIVATE /W4)
else()
    target_compile_options(test_arpx_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME arpx_protocol COMMAND test_arpx_protocol)

add_executable(test_peer_transport_contract
    tests/test_peer_transport_contract.c
)
target_link_libraries(test_peer_transport_contract PRIVATE
    agent_peer_transport_contract
)

if(MSVC)
    target_compile_options(test_peer_transport_contract PRIVATE /W4)
else()
    target_compile_options(test_peer_transport_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME peer_transport_contract COMMAND test_peer_transport_contract)

add_executable(test_ipc_protocol tests/test_ipc_protocol.c)
target_link_libraries(test_ipc_protocol PRIVATE agent_ipc_protocol)

if(MSVC)
    target_compile_options(test_ipc_protocol PRIVATE /W4)
else()
    target_compile_options(test_ipc_protocol PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME ipc_protocol COMMAND test_ipc_protocol)

add_executable(test_public_ipv6 tests/test_public_ipv6.c)
target_link_libraries(test_public_ipv6 PRIVATE agent_public_ipv6)
target_include_directories(agent_public_ipv6 PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
)
if(MSVC)
    target_compile_options(test_public_ipv6 PRIVATE /W4)
else()
    target_compile_options(test_public_ipv6 PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME public_ipv6 COMMAND test_public_ipv6)

add_executable(test_public_ingress tests/test_public_ingress.c)
target_link_libraries(test_public_ingress PRIVATE agent_public_ingress)
if(MSVC)
    target_compile_options(test_public_ingress PRIVATE /W4)
else()
    target_compile_options(test_public_ingress PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME public_ingress COMMAND test_public_ingress)

add_executable(test_edge_proxy tests/test_edge_proxy.c)
target_link_libraries(test_edge_proxy PRIVATE agent_edge_proxy)
if(MSVC)
    target_compile_options(test_edge_proxy PRIVATE /W4)
else()
    target_compile_options(test_edge_proxy PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME edge_proxy COMMAND test_edge_proxy)

find_path(NEXUS_MBEDTLS_INCLUDE_DIR mbedtls/md.h)
find_library(NEXUS_MBEDCRYPTO_LIBRARY mbedcrypto)
if(NEXUS_MBEDTLS_INCLUDE_DIR AND NEXUS_MBEDCRYPTO_LIBRARY)
    set(NEXUS_LAN_SESSION_CRYPTO ${NEXUS_MBEDCRYPTO_LIBRARY})
else()
    find_package(OpenSSL QUIET COMPONENTS Crypto)
    if(OpenSSL_Crypto_FOUND)
        set(NEXUS_LAN_SESSION_CRYPTO OpenSSL::Crypto)
        set(NEXUS_LAN_SESSION_OPENSSL TRUE)
    endif()
endif()
if(NEXUS_LAN_SESSION_CRYPTO)
    add_library(agent_lan_session STATIC
        feed/common/agent_lan_session.c
    )
    target_include_directories(agent_lan_session PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/feed/common
    )
    target_link_libraries(agent_lan_session PUBLIC
        ${NEXUS_LAN_SESSION_CRYPTO}
    )
    if(NEXUS_LAN_SESSION_OPENSSL)
        target_compile_definitions(agent_lan_session PRIVATE
            NEXUS_LAN_SESSION_USE_OPENSSL=1
        )
    else()
        target_include_directories(agent_lan_session PRIVATE
            ${NEXUS_MBEDTLS_INCLUDE_DIR}
        )
    endif()
    add_executable(test_lan_session tests/test_lan_session.c)
    target_link_libraries(test_lan_session PRIVATE agent_lan_session)
    if(MSVC)
        target_compile_options(test_lan_session PRIVATE /W4)
    else()
        target_compile_options(test_lan_session PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
        )
    endif()
    add_test(NAME lan_session COMMAND test_lan_session)
endif()

add_executable(test_rate_limiter tests/test_rate_limiter.c)
target_link_libraries(test_rate_limiter PRIVATE agent_rate_limiter)

if(MSVC)
    target_compile_options(test_rate_limiter PRIVATE /W4)
else()
    target_compile_options(test_rate_limiter PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME rate_limiter COMMAND test_rate_limiter)

add_executable(test_tenant_quota tests/test_tenant_quota.c)
target_link_libraries(test_tenant_quota PRIVATE agent_tenant_quota)
if(MSVC)
    target_compile_options(test_tenant_quota PRIVATE /W4)
else()
    target_compile_options(test_tenant_quota PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME tenant_quota COMMAND test_tenant_quota)

add_executable(test_gateway_contract tests/test_gateway_contract.c)
target_link_libraries(test_gateway_contract PRIVATE agent_gateway_contract)

if(MSVC)
    target_compile_options(test_gateway_contract PRIVATE /W4)
else()
    target_compile_options(test_gateway_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME gateway_contract COMMAND test_gateway_contract)

add_executable(test_auth_policy tests/test_auth_policy.c)
target_link_libraries(test_auth_policy PRIVATE
    agent_auth_policy agent_gateway_contract
)

if(MSVC)
    target_compile_options(test_auth_policy PRIVATE /W4)
else()
    target_compile_options(test_auth_policy PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME auth_policy COMMAND test_auth_policy)

add_executable(test_replay_cache tests/test_replay_cache.c)
target_link_libraries(test_replay_cache PRIVATE agent_replay_cache)

if(MSVC)
    target_compile_options(test_replay_cache PRIVATE /W4)
else()
    target_compile_options(test_replay_cache PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME replay_cache COMMAND test_replay_cache)

add_executable(test_stream_resume_cache tests/test_stream_resume_cache.c)
target_link_libraries(test_stream_resume_cache PRIVATE agent_stream_resume_cache)

if(MSVC)
    target_compile_options(test_stream_resume_cache PRIVATE /W4)
else()
    target_compile_options(test_stream_resume_cache PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME stream_resume_cache COMMAND test_stream_resume_cache)

add_executable(test_invoke_contract tests/test_invoke_contract.c)
target_link_libraries(test_invoke_contract PRIVATE agent_invoke_contract)

if(MSVC)
    target_compile_options(test_invoke_contract PRIVATE /W4)
else()
    target_compile_options(test_invoke_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME invoke_contract COMMAND test_invoke_contract)

add_executable(test_adapter_contract tests/test_adapter_contract.c)
target_link_libraries(test_adapter_contract PRIVATE agent_adapter_contract)

if(MSVC)
    target_compile_options(test_adapter_contract PRIVATE /W4)
else()
    target_compile_options(test_adapter_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME adapter_contract COMMAND test_adapter_contract)

add_executable(test_agent_dynamic_manifest
    tests/test_agent_dynamic_manifest.c
)
target_link_libraries(test_agent_dynamic_manifest PRIVATE
    agent_dynamic_manifest
)
if(MSVC)
    target_compile_options(test_agent_dynamic_manifest PRIVATE /W4)
else()
    target_compile_options(test_agent_dynamic_manifest PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()
add_test(NAME agent_dynamic_manifest COMMAND test_agent_dynamic_manifest)

add_executable(test_adapter_ingress_contract
    tests/test_adapter_ingress_contract.c
)
target_link_libraries(test_adapter_ingress_contract PRIVATE
    agent_adapter_ingress_contract agent_invoke_contract
)

if(MSVC)
    target_compile_options(test_adapter_ingress_contract PRIVATE /W4)
else()
    target_compile_options(test_adapter_ingress_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME adapter_ingress_contract COMMAND test_adapter_ingress_contract)

add_executable(test_sse_contract tests/test_sse_contract.c)
target_link_libraries(test_sse_contract PRIVATE agent_sse_contract)

if(MSVC)
    target_compile_options(test_sse_contract PRIVATE /W4)
else()
    target_compile_options(test_sse_contract PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    )
endif()

add_test(NAME sse_contract COMMAND test_sse_contract)

# The standalone source release omits SDK and lab orchestration.
set(NEXUS_INTEGRATION_DEFAULT OFF)
if(EXISTS "${CMAKE_SOURCE_DIR}/sdk/nexus-agent-sdk-python/tests")
    set(NEXUS_INTEGRATION_DEFAULT ON)
endif()
option(NEXUS_INTEGRATION_TESTS "Include SDK and private lab integration contracts" ${NEXUS_INTEGRATION_DEFAULT})
message(STATUS "SDK/lab integration tests: ${NEXUS_INTEGRATION_TESTS}")
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME luci_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_luci_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(luci_contract PROPERTIES LABELS "p7.3;p7.4;p7.5;p8.2;luci")

    add_test(NAME role_services_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_role_services_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(role_services_contract PROPERTIES LABELS "p7.6;openwrt;roles")

    add_test(NAME directory_dns_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_directory_dns_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(directory_dns_contract PROPERTIES
        LABELS "p8.11;openwrt;directory;dns;luci")

    add_test(NAME cross_resolver_autodetect_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_cross_resolver_autodetect_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(cross_resolver_autodetect_contract PROPERTIES
        LABELS "p3.6;openwrt;dnssec;luci")

    add_test(NAME agent_access_proxy_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_agent_access_proxy_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(agent_access_proxy_contract PROPERTIES
        LABELS "p8.1;agent-access-proxy;registration")

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME public_ipv6_contract
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_public_ipv6_contract.py
                ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(public_ipv6_contract PROPERTIES
            LABELS "p8.12;openwrt;public-ipv6;luci")
    endif()

    add_test(NAME public_ingress_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_public_ingress_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(public_ingress_contract PROPERTIES
        LABELS "p8.12.2;openwrt;public-ipv6")

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME no_jwt_mode_contract
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_no_jwt_mode_contract.py
                ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(no_jwt_mode_contract PROPERTIES
            LABELS "p8.21;openwrt;no-jwt;lan;relay;public-ipv6")
    endif()

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME automatic_https_registration_contract
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_automatic_https_registration_contract.py
                ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(automatic_https_registration_contract PROPERTIES
            LABELS "p8.26;openwrt;python-sdk;https;luci")
    endif()

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME nexus_cloud_relay_mcp_contract
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_nexus_cloud_relay_mcp_contract.py
                ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(nexus_cloud_relay_mcp_contract PROPERTIES
            LABELS "openwrt;relay;cloud;mcp;e2e;contract"
            TIMEOUT 30)
    endif()

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME nexus_cloud_relay_stage_state
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_nexus_cloud_relay_stage_state.py
                ${CMAKE_SOURCE_DIR}
        )
        set_tests_properties(nexus_cloud_relay_stage_state PROPERTIES
            LABELS "openwrt;relay;cloud;mcp;e2e;staged;contract"
            TIMEOUT 120)
    endif()

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME nexus_cloud_relay_mtls_probe
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/test_nexus_cloud_relay_mtls_probe.py
        )
        set_tests_properties(nexus_cloud_relay_mtls_probe PROPERTIES
            LABELS "openwrt;relay;cloud;mtls;security"
            TIMEOUT 90)
    endif()

    add_test(NAME relay_https_gateway_contract
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_SOURCE_DIR}/tests/test_relay_https_gateway_contract.py
            ${CMAKE_SOURCE_DIR}
    )
    set_tests_properties(relay_https_gateway_contract PROPERTIES
        LABELS "openwrt;relay;gateway;https;security;contract"
        TIMEOUT 30)

    if(NEXUS_INTEGRATION_TESTS)
        add_test(NAME nexus_agent_sdk_python
            COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_SOURCE_DIR}/sdk/nexus-agent-sdk-python/tests
                -p test_*.py
        )
        set_tests_properties(nexus_agent_sdk_python PROPERTIES
            LABELS "p8.1;p8.2;p8.3;p8.4;python-sdk")
    endif()
endif()
