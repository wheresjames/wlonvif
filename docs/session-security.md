# Session and transport security contracts

`DeviceSession` is the authoritative owner of cancellation, credential-provider
ownership, and admission of new work. Applications should call `close()` for
deterministic shutdown; destruction is only a non-blocking cancellation and
resource-release backstop.

## Session state

| Current | Event | Result | Next |
|---|---|---|---|
| open | operation starts | operation is linked to session cancellation | open |
| open | close starts | reject new work, cancel children, run the PTZ close sweep, then close provider | closing |
| closing | operation starts | `closed_handle` | closing |
| closing | final child is accounted | complete close waiters once | closed |
| closing | caller cleanup budget expires | report `cleanup_complete=false`; accounting continues | closing |
| closed | close starts | return the shared terminal result | closed |
| closed | operation starts | `closed_handle` | closed |

Session cancellation never applies to a PTZ Stop. The close sweep sends its Stops
after cancelling the session's children, and those Stops authenticate with the
session's credentials, so the credential provider is closed only after the sweep
finishes. Cancellation tokens link transitively: linking an already linked token
keeps every source it observed.

## Connection authorization

Endpoint policy and host permission are deliberately separate checks.

| Check | Input | Location | Denial category |
|---|---|---|---|
| URL policy | effective URL and original origin | before request construction | `endpoint_rejected` |
| Host permission | original/effective URL, normalized host/port, complete resolved set, proposed address, purpose, provenance, attempt ID | after the transport's single resolution and before socket creation | `host_permission_denied` |

The curl transport pins the approved address with `CURLOPT_RESOLVE`; curl therefore
does not perform a second lookup or select an address outside the approved set.
An IP-literal host (IPv4, or IPv6 without its URL brackets) is presented to the
authorizer as its own single address and needs no pin, since it cannot be rebound.
Every new request, redirect implementation, endpoint replacement, or reconnect is
a new attempt and must receive a new opaque attempt ID. Redirect following is
disabled in the default transport.

## Credential lifetime

```text
fixed provider owns secret buffers
        |
        +-- acquire(deadline, cancellation)
                  |
                  +-- independent move-only lease for one operation
                              |
                              +-- SOAP digest / bounded HTTP challenge sequence
                              |
                              +-- best-effort erase on lease destruction
```

Provider close rejects acquisition and best-effort erases provider-owned buffers.
Existing leases remain valid until their operation finishes or session cancellation
ends it. Erasure is best effort, not a forensic guarantee: allocators, TLS stacks,
operating systems, or devices may retain copies outside this API's ownership.

## Structured failures

`Error` keeps its stable category, operation, retryability, and uncertain-delivery
fields. `http`, `soap_fault`, and `host_permission` contain optional owned detail.
SOAP strings are bounded to 512 bytes per field. Diagnostic prose is sanitized and
must not be parsed for control flow. Device-busy and rate-limit classifications are
not inferred from arbitrary prose.

## PTZ movement ledger

The session owns one entry per profile. The ledger lock guards only ledger state
and is never held across a network request, so a Stop is never queued behind an
in-flight movement. Each entry records when it was accepted: a Stop removes only
entries accepted before it was sent, status reconciliation removes only entries
older than the status request, and a movement that lands after a Stop for its
profile (or after close began) is stopped again. The close sweep first waits,
within its budget, for in-flight movements to land.

| Event | Existing entry | Resulting entry | Safety action |
|---|---|---|---|
| continuous move accepted | any | continuous | none |
| relative, absolute, or preset move accepted | any | finite | none |
| move delivery uncertain | any | initiating movement | bounded Stop |
| move rejected before delivery | any | unchanged | none |
| move cancelled or timed out | any | retained if delivery may have occurred | bounded Stop |
| move accepted after a concurrent Stop or after close began | any | initiating movement | bounded Stop |
| successful applicable Stop | any | removed | complete |
| authoritative stopped status | any | removed | none |
| request completion without stopped proof | finite | finite | none |
| session or runtime shutdown | any | removed only after successful Stop | bounded Stop per profile |

A later movement for the same profile supersedes the ledger entry, but the close
sweep still issues exactly one applicable Stop. Movement requests are marked
non-idempotent and are never automatically retried.

## Error mapping

| Explicit signal | Category | Typed detail |
|---|---|---|
| HTTP 401 | `authentication` | HTTP status |
| HTTP 403 | `authorization` | HTTP status |
| HTTP 429 | `rate_limited` | HTTP status |
| other non-2xx without a SOAP Fault | `protocol_violation` | HTTP status |
| SOAP `ter:DeviceBusy` | `device_busy` | bounded SOAP fault |
| SOAP `ter:RateLimit` or `ter:TooManyRequests` | `rate_limited` | bounded SOAP fault |
| other SOAP Fault | `soap_fault` | bounded SOAP fault |
| failed safety Stop | initiating outcome remains authoritative | `SafetyActionDetail` with redacted category/code |

Unknown vendor prose retains the broader native category. A failed Stop never
replaces the initiating movement error.
