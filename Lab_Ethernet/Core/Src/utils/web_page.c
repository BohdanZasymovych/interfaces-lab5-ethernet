#include "utils/web_page.h"

/*
 * TEMPORARY page: shows the raw /status JSON, refreshed once per second.
 * Replace it with the real dashboard (GPS, T/P/RH/Lum, stale badges).
 */
const char WEB_INDEX_HTML[] = "<!doctype html>\n"
                              "<html lang=\"en\">\n"
                              "<head>\n"
                              "<meta charset=\"utf-8\">\n"
                              "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                              "<title>Device panel</title>\n"
                              "<style>body{font-family:monospace;margin:16px}#err{color:#c00}</style>\n"
                              "</head>\n"
                              "<body>\n"
                              "<h1>Device panel</h1>\n"
                              "<p id=\"err\"></p>\n"
                              "<pre id=\"out\">loading...</pre>\n"
                              "<script>\n"
                              "const out = document.getElementById('out');\n"
                              "const err = document.getElementById('err');\n"
                              "async function poll() {\n"
                              "  try {\n"
                              "    const r = await fetch('/status', {cache: 'no-store'});\n"
                              "    out.textContent = JSON.stringify(await r.json(), null, 2);\n"
                              "    err.textContent = '';\n"
                              "  } catch (e) {\n"
                              "    err.textContent = 'device not reachable';\n"
                              "  }\n"
                              "}\n"
                              "poll();\n"
                              "setInterval(poll, 1000);\n"
                              "</script>\n"
                              "</body>\n"
                              "</html>\n";

const size_t WEB_INDEX_HTML_LEN = sizeof(WEB_INDEX_HTML) - 1;
