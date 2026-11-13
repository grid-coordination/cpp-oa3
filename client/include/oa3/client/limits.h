#pragma once

// Capacities of oa3_client. Each may be overridden with a -D build flag.

// A request URL without its NUL: the base URL, path and query, percent-encoded.
#ifndef OA3_URL_MAX
#define OA3_URL_MAX 511
#endif

// One log line without its NUL; a longer line is truncated.
#ifndef OA3_LOG_LINE_MAX
#define OA3_LOG_LINE_MAX 159
#endif

// The limit sent on each /programs page; openadr3.yaml caps it at 50.
#ifndef OA3_PROGRAMS_PAGE_LIMIT
#define OA3_PROGRAMS_PAGE_LIMIT 50
#endif

// Pages read while resolving a programName before giving up as not found.
#ifndef OA3_PROGRAM_PAGES_MAX
#define OA3_PROGRAM_PAGES_MAX 200
#endif

// The limit sent on each /events page.
#ifndef OA3_EVENTS_PAGE_LIMIT
#define OA3_EVENTS_PAGE_LIMIT 50
#endif

// Pages read per events fetch; a fetch that stops here reports kTruncated.
#ifndef OA3_EVENT_PAGES_MAX
#define OA3_EVENT_PAGES_MAX 20
#endif

static_assert(OA3_PROGRAMS_PAGE_LIMIT >= 1 && OA3_PROGRAMS_PAGE_LIMIT <= 50,
              "openadr3.yaml limit is 0..50");
static_assert(OA3_EVENTS_PAGE_LIMIT >= 1 && OA3_EVENTS_PAGE_LIMIT <= 50,
              "openadr3.yaml limit is 0..50");
static_assert(OA3_EVENT_PAGES_MAX >= 1, "at least one /events page");
