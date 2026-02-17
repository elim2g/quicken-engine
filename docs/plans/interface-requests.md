# Interface Change Requests

Agents: append your requests below using the template in WORKFLOW.md Section 8.2.
The Principal Engineer reviews these during Phase 4 merges.

---

### [REQUEST] Add qk_net_server_get_client_state()

**Requesting agent**: Gameplay
**Date**: 2026-02-16
**Priority**: Blocking
**Header**: include/netcode/qk_netcode.h

**Current signature / type**:
```c
// NEW -- no existing function
```

**Proposed change**:
```c
/* Query the connection state of a specific client slot.
 * Returns QK_CONN_DISCONNECTED if the slot is unused. */
qk_conn_state_t qk_net_server_get_client_state(u8 client_id);
```

**Rationale**: For mid-session join (task #3) and the connect command (task #4),
`main.c` needs to detect when remote clients connect or disconnect so it can
call `qk_game_player_connect()` and `qk_game_player_disconnect()` for them.
The existing server API has no per-client state query. Without this, there is
no reliable way to detect new remote connections.

**Workaround**: Track a `bool s_client_was_connected[QK_MAX_PLAYERS]` array in
`main.c` and use `qk_net_server_client_count()` changes to trigger a scan,
using `qk_net_server_get_input()` as a proxy for "connected". Fragile because
`get_input` can return false for connected clients that haven't sent input yet.

**Resolution**: **Approved & Implemented** (2026-02-16). Both `qk_net_server_get_client_state()` and `qk_net_server_is_client_map_ready()` are declared in `include/netcode/qk_netcode.h:79-80` and implemented in `src/netcode/netcode.c:117-130`. See `src/server_main.c:detect_remote_players()` for usage example.

---
