/*
 * Test header coexistence and include order portability.
 * Must compile cleanly with sdk.h first, or wire/link first.
 */

#include "mcl/sdk.h"
#include "mcl/wire.h"
#include "mcl/link.h"

#include <stdio.h>

int main(void)
{
    mcl_node_t node;
    mcl_node_config_t config;
    mcl_wire_tier0_t obj;
    mcl_link_context_key_t key;

    (void)node;
    (void)config;
    (void)obj;
    (void)key;

    puts("mcl_sdk header coexistence: PASS");
    return 0;
}
