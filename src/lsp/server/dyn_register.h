#ifndef IRON_LSP_SERVER_DYN_REGISTER_H
#define IRON_LSP_SERVER_DYN_REGISTER_H

/* Phase 2 Plan 03 Task 02 -- client/registerCapability outbound.
 *
 * Post-`initialized` the server sends `client/registerCapability` for
 * `workspace/didChangeWatchedFiles` with globs for `.iron`, `iron.toml`,
 * `iron.lock`. The client then returns a response we don't care about
 * (dyn_register is fire-and-forget for watched-files registration).
 *
 * Kept deliberately small; Plan 06 may extend for other dynamic
 * registrations (file operations, didChangeConfiguration) -- register
 * new entry points alongside ilsp_dyn_register_watched_files, don't
 * overload this one. */

typedef struct IronLsp_Server IronLsp_Server;

/* Enqueue a `client/registerCapability` outbound request for the watched
 * files we care about. Uses server->writer at ILSP_PRIO_RESPONSE
 * priority (request-shaped -- never dropped under back-pressure).
 * Generates the request id via the server->next_request_id counter. */
void ilsp_dyn_register_watched_files(IronLsp_Server *server);

#endif /* IRON_LSP_SERVER_DYN_REGISTER_H */
