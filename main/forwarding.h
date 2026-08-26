/*
 * forwarding.h — Stage 3 multi-hop DATA forwarding.
 *
 * Responsibilities:
 *   - DATA packet handling (receive dispatcher callback)
 *   - Destination check (local delivery vs forward)
 *   - Duplicate detection
 *   - Hop-count enforcement
 *   - Static next-hop lookup
 *   - Active-neighbor validation
 *   - DATA generation (NODE_A only)
 */
#ifndef FORWARDING_H
#define FORWARDING_H

#include "espnow_manager.h"

/* Called by the dispatcher when a DATA packet is received. */
void forwarding_handle_data(const espnow_evt_recv_t *evt);

/* Start the DATA generator task (NODE_A only).
 * On other roles this is a no-op. */
void forwarding_start(void);

/* Print the startup banner with role/MAC validation.
 * Returns true if the role/MAC match is valid. */
bool forwarding_validate_role(void);

#endif /* FORWARDING_H */
