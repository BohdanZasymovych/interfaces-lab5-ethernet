#ifndef WEB_PAGE_H
#define WEB_PAGE_H

/*
 * The page served at GET /. One self-contained HTML file (CSS and JS inline,
 * no build step) stored in flash as a string. It polls /status once per second
 * and updates only the text of the value elements, so nothing flickers.
 */

#include <stddef.h>

extern const char WEB_INDEX_HTML[];
extern const size_t WEB_INDEX_HTML_LEN;  // strlen(WEB_INDEX_HTML), computed at compile time

#endif
