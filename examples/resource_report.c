/* Machine-readable resource envelope for the portable data structures. */
#include "mcl/machine.h"
#include "mcl/ap_listen.h"

#include <stdio.h>

int main(void)
{
    mcl_ap_listen_config_t listen;

    mcl_ap_listen_default_config(&listen);
    listen.max_payload_bytes = 17u;

    printf("mcl_node_bytes=%lu\n", (unsigned long)sizeof(mcl_node_t));
    printf("mcl_rendezvous_bytes=%lu\n", (unsigned long)sizeof(mcl_rdv_t));
    printf("mcl_machine_bytes=%lu\n", (unsigned long)sizeof(mcl_machine_t));
    printf("ap_modem_scratch_bytes=%lu\n",
           (unsigned long)sizeof(mcl_ap_modem_scratch_t));
    printf("ap_listener_state_bytes=%lu\n",
           (unsigned long)sizeof(mcl_ap_listener_t));
    printf("ap_min_window_pcm16_samples=%lu\n",
           (unsigned long)mcl_ap_listen_min_window_samples(&listen));
    printf("ap_min_window_bytes=%lu\n",
           (unsigned long)(2u * mcl_ap_listen_min_window_samples(&listen)));
    return 0;
}
