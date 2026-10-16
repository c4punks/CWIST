# cde-json-viewer example

A CDE-style (Common Desktop Environment) system monitor page, built
entirely in C: a mock JSON document is parsed with cJSON, turned into an
HTML table with the HTML builder (`cwist_html_element_*`), and styled
with a generated stylesheet from the CSS composer
(`cwist_css_generate_stylesheet`).

The single route `GET /` renders the page; there is no other endpoint
and no external asset — the CSS is inlined into a `<style>` tag.

## Build

Build the library first, from the repository root:

```sh
make
```

Then build the example:

```sh
make -C example/cde-json-viewer
```

The directory Makefile produces `cde_json_viewer`. The root Makefile
also has a rule for the same program under a dashed name:

```sh
make example/cde-json-viewer/cde-json-viewer
```

`make examples-check` builds it as well.

## Run

```sh
./example/cde-json-viewer/cde_json_viewer     # or ./example/cde-json-viewer/cde-json-viewer
```

The server listens on port 8080. Open <http://localhost:8080/> in a
browser, or fetch the page with curl:

```sh
curl http://localhost:8080/
```

The response is a `text/html` page titled "System Monitor": a table with
one row per string field of the embedded mock JSON (host `sun-sparc-station`,
uptime `42 days`, load average, and so on), styled with the generated
CSS variables in a Solaris-ish blue palette.

## Clean

```sh
make -C example/cde-json-viewer clean
rm -f example/cde-json-viewer/cde-json-viewer
```
