# HTML 컴포넌트, 범위 CSS, 프래그먼트와 자산

*헤더:* `<cwist/core/html/component.h>`, `<cwist/core/html/css_composer.h>`

컴포넌트는 `<cwist/core/html/builder.h>`의 요소 빌더 위에 있는, 이름이 붙은 렌더 함수입니다. CSS
범위(scope)는 컴포넌트 하나에 고유한 클래스 이름을 나눠 주고, 실제로 쓰인 클래스의 규칙만
내보냅니다. 둘 다 소켓이나 스레드 의존성이 없는 평범한 C이며 WASM 빌드에 들어 있습니다.

## 컴포넌트

### `cwist_html_component_create` / `cwist_html_component_destroy`
```c
typedef cwist_html_element_t *(*cwist_html_component_render_fn)(const void *props,
                                                                cwist_html_element_t **children,
                                                                size_t child_count);

cwist_html_component_t *cwist_html_component_create(const char *name,
                                                    cwist_html_component_render_fn render_fn);
void cwist_html_component_destroy(cwist_html_component_t *comp);
const char *cwist_html_component_name(const cwist_html_component_t *comp);
```
`name`은 비어 있지 않아야 하며 복사됩니다. 컴포넌트를 파괴해도 이미 만들어 낸 요소 트리에는
영향이 없습니다.

### `cwist_html_component_instantiate`
```c
cwist_html_element_t *cwist_html_component_instantiate(cwist_html_component_t *comp,
                                                       const void *props,
                                                       cwist_html_element_t **children,
                                                       size_t child_count);
```
렌더 함수를 실행하고 그 루트 요소를 반환합니다. 결과는 `cwist_html_element_create()`의 결과와
똑같이 호출자가 소유합니다. `cwist_html_element_destroy()`로 파괴하거나 부모에 붙이세요.

호출자는 이 함수를 호출할 때 `children`의 모든 요소를 넘겨주며, 이후에 그것들을 쓰면 안 됩니다.
요소들은 렌더 함수에 넘겨지거나, `comp`가 NULL이면 여기서 파괴됩니다. 각 요소는 한 번만 나열되어야
하고, 다른 항목의 트리를 포함해 이미 다른 트리에 속해 있으면 안 됩니다. `children`은
`child_count`가 0일 때만 NULL일 수 있습니다. NULL 배열에 0이 아닌 개수를 주면 아무것도 렌더링하거나
해제하지 않고 NULL을 반환합니다. 개별 항목은 NULL이어도 됩니다.

렌더 함수는 받은 자식들을 소유합니다. 각 자식은 성공하든 실패하든, 반환하는 트리 안에 들어가거나
(`cwist_html_element_add_child()`로 붙이거나 루트로 반환) 렌더 함수가 파괴해야 합니다. 실패 시
부분적으로 만든 트리를 파괴하면 이미 붙은 자식도 함께 해제되므로, 붙지 않은 것만
`cwist_html_element_destroy()`가 필요합니다.

`props`는 손대지 않고 그대로 전달되므로, 컴포넌트가 자기만의 props 구조체를 정의합니다.

## 범위 CSS

### `cwist_css_scope_init` / `cwist_css_scope_destroy`
```c
void cwist_css_scope_init(cwist_css_scope *scope, const char *component_name);
void cwist_css_scope_destroy(cwist_css_scope *scope);
```
범위는 호출자 저장소에 있는 값 구조체입니다. 범위가 나눠 주는 모든 클래스에는 `-XXXXXXXX` 접미사가
붙는데, 이는 `component_name`의 32비트 FNV-1a 해시를 소문자 16진수로 쓴 것입니다. 해시는 의도적으로
시드를 쓰지 않습니다. 같은 컴포넌트 이름은 모든 프로세스와 모든 실행에서 같은 클래스 이름을 주므로,
마크업과 스타일시트가 서로 다른 prefork 워커에서 와도 됩니다. 보안 경계가 아니며, 원칙적으로 두
컴포넌트 이름이 같은 접미사를 가질 수 있습니다.

`cwist_css_scope_destroy()`는 모든 것을 해제하고, 반환된 클래스 이름을 모두 무효화하며, 범위를
비웁니다. 두 번 호출해도 무해합니다. 파괴되었거나 0으로 초기화된 범위는 `cwist_css_scope_init()`을
다시 호출할 때까지 새 클래스와 규칙을 거부합니다.

### `cwist_css_scope_class`
```c
const char *cwist_css_scope_class(cwist_css_scope *scope, const char *base_class);
```
범위가 붙은 이름(`"btn"` -> `"btn-a1b2c3d4"`)을 반환하고 그 클래스를 사용됨으로 표시합니다. 같은
기본 클래스는 항상 범위가 소유하는 같은 포인터를 반환합니다. `base_class`는 평범한 ASCII
식별자여야 합니다. 문자나 `_`(앞에 `-` 하나가 선택적으로 올 수 있음)로 시작하고, 그 뒤에 문자,
숫자, `_`, `-`가 옵니다. 이는 CSS 식별자 문법의 의도적인 부분 집합입니다. 이스케이프, 비 ASCII
문자, `--` 이름은 거부됩니다. 그 밖의 입력은 NULL을 반환합니다.

### `cwist_css_scope_add_rule`
```c
int cwist_css_scope_add_rule(cwist_css_scope *scope, const char *base_class,
                             const char *declarations);
```
클래스에 선언 블록 본문을 붙이며, 이전 것이 있으면 바꿉니다. 성공하면 0, 잘못된 입력이나 할당
실패면 -1을 반환합니다.

선언은 애플리케이션이 작성한 신뢰할 수 있는 CSS이며 그대로 출력됩니다. 확인하는 문자는 두 가지입니다.
`<`는 거부되므로 생성된 스타일시트가 둘러싼 `<style>` 요소를 끝낼 수 없습니다. 중첩 블록을 잡기
위해 `{`와 `}`도 거부됩니다. 그 밖에는 검증하지 않습니다. 닫히지 않은 주석이나 문자열이 그 뒤의
규칙에 영향을 줄 수 있습니다. 신뢰할 수 없는 입력으로 선언을 만들지 마세요.

### `cwist_css_scope_generate_stylesheet`
```c
cwist_sstring *cwist_css_scope_generate_stylesheet(const cwist_css_scope *scope);
```
`cwist_css_scope_class()`로 요청되었고 규칙도 받은 각 클래스에 대해, 범위가 각 클래스를 처음 본
순서대로 `.<scoped> { <declarations> }`를 내보냅니다. 한 번도 요청되지 않은 클래스의 규칙은
빠집니다. 반환된 문자열은 호출자가 파괴합니다. `scope`가 NULL이거나 할당이 실패하면 NULL을
반환하며, 일부만 만든 스타일시트를 반환하는 일은 없습니다.

평범한 클래스 선택자만 생성합니다. 가상 클래스, 하위 선택자, 미디어 쿼리는 범위 API가 다루지
않습니다.

## HTML over the wire

*헤더:* `<cwist/net/http/html_response.h>`

이 도우미를 쓰면 핸들러 하나가 같은 URL에 대해 일반 탐색과 프래그먼트 요청을 모두 처리할 수
있습니다. 클라이언트 쪽 스크립트가 없으면 모든 링크와 폼이 전체 페이지 로드로 동작하고, 프래그먼트를
교체하는 클라이언트가 있으면 같은 핸들러가 바뀌는 부분만 반환합니다.

요청이 `HX-Request: true`(htmx)를 담고 있으면(htmx 기록 복원
`HX-History-Restore-Request: true`는 제외), 또는 비어 있지 않은 `Turbo-Frame` 헤더(Hotwire Turbo
프레임)가 있으면 프래그먼트 요청입니다. 두 라이브러리 모두 필요하지 않으며 함께 들어 있지도
않습니다.

### `cwist_http_request_wants_fragment` / `cwist_http_request_fragment_target`
```c
bool cwist_http_request_wants_fragment(const cwist_http_request *req);
const char *cwist_http_request_fragment_target(const cwist_http_request *req);
```
대상은 `Turbo-Frame` 값, 없으면 `HX-Target`이며, 프래그먼트 요청이 아니면 NULL입니다.

### `cwist_http_response_set_view`
```c
int cwist_http_response_set_view(const cwist_http_request *req, cwist_http_response *res,
                                 cwist_html_element_t *content, cwist_html_component_t *layout,
                                 const void *layout_props);
```
프래그먼트 요청은 `content`만 받습니다. 다른 요청은 `<!DOCTYPE html>`로 시작하는 전체 문서를
받습니다. `content`를 `layout` 컴포넌트의 유일한 자식으로 넘긴 것, 또는 `layout`이 NULL이면
`content` 자체입니다. 두 응답 모두 `Content-Type: text/html; charset=utf-8`과 `Vary: HX-Request,
HX-History-Restore-Request, HX-Target, Turbo-Frame`을 받으므로, HTTP 캐시가 두 표현을 구분해
둡니다. 같은 이유로 프로세스 내 응답 캐시(Big Dumb Reply)는 `Vary`가 있는 응답을 학습하지
않습니다. 상태 코드는 핸들러가 설정한 그대로 둡니다.

`content`는 항상 소비됩니다. 전체 페이지 응답에서는 레이아웃의 렌더 함수로 넘어가며, 렌더 함수가
일반적인 컴포넌트 규칙에 따라 소유합니다.

### `cwist_http_response_set_html`
```c
int cwist_http_response_set_html(cwist_http_response *res, cwist_html_element_t *root,
                                 bool document);
```
더 낮은 수준의 단계입니다. `root`(항상 소비됨)를 본문에 렌더링하고, `document`가 참이면
doctype을 붙이며, HTML 콘텐츠 타입을 설정합니다. 이미 포인터, 파일, 스트림 본문을 쓰는 응답은
거부합니다.

### `cwist_http_response_add_oob`
```c
int cwist_http_response_add_oob(const cwist_http_request *req, cwist_http_response *res,
                                cwist_html_element_t *el);
```
프래그먼트 응답에 두 번째 영역을 추가합니다. 예를 들어 페이지의 다른 곳에 있는 카운터나 알림입니다.
프래그먼트 요청이면 요소(`id`가 있어야 함)를 `hx-swap-oob="true"`와 함께 본문 뒤에 붙입니다. 이미
`hx-swap-oob` 값이 있으면 그대로 둡니다. 전체 페이지 요청이면 버립니다. 페이지에 이미 그 영역이
들어 있기 때문입니다. htmx는 대역 외(out-of-band) 요소를 적용하고, Turbo 프레임은 무시합니다. 주
콘텐츠를 설정한 뒤에 호출하세요.

### `cwist_http_response_html_redirect`
```c
int cwist_http_response_html_redirect(const cwist_http_request *req, cwist_http_response *res,
                                      const char *location);
```
htmx 요청은 `HX-Redirect: <location>`과 함께 `200`을 받습니다. 스크립트가 보내는 요청은 3xx를
스스로 따라가므로, 평범한 리다이렉트는 다음 페이지를 프래그먼트 자리에 넣어 버리기 때문입니다.
Turbo 프레임을 포함한 그 밖의 모든 요청은 `Location`과 함께 `303 See Other`를 받으며, 이는 폼
POST를 GET으로 바꾸기도 합니다. 본문은 비워지고 `Vary: HX-Request`가 추가됩니다. 제어 문자(특히
CR과 LF)가 들어 있는 location은 -1로 거부되며 아무것도 바뀌지 않습니다.

### 핸들러 예제

```c
static cwist_html_component_t *layout; /* created once at startup */

static void items(cwist_http_request *req, cwist_http_response *res) {
    cwist_html_element_t *list = cwist_html_element_create("ul");
    cwist_html_element_set_id(list, "items");
    /* ... add <li> children ... */
    if (cwist_http_response_set_view(req, res, list, layout, "Items") != 0) {
        res->status_code = CWIST_HTTP_INTERNAL_ERROR;
    }
}

static void add_item(cwist_http_request *req, cwist_http_response *res) {
    /* ... store the item ... */
    cwist_http_response_html_redirect(req, res, "/items");
}
```

`<a href="/items" hx-get="/items" hx-target="#items">`라면, htmx가 없는 브라우저는 링크를 따라가
전체 페이지를 받고, htmx는 같은 핸들러에서 `<ul id="items">` 프래그먼트만 받습니다.

## 자산 파이프라인

*헤더:* `<cwist/core/html/css_composer.h>`, `<cwist/sys/app/assets.h>`

시작 시 만든 스타일시트(`cwist_css_config`, 컴포넌트 범위, 파일에서)는 별도의 프런트엔드 빌드 단계
없이 묶고, 최소화하고, 콘텐츠 해시가 들어간 URL로 제공할 수 있습니다.

### `cwist_css_minify` / `cwist_css_bundle`
```c
cwist_sstring *cwist_css_minify(const char *css);
cwist_sstring *cwist_css_bundle(const char *const *parts, size_t count, bool minify);
```
최소화기는 의도적으로 보수적입니다. `!`로 시작하는 주석(라이선스 고지)을 제외한 주석을 지우고,
CSS가 공백을 필요로 할 수 없는 곳에서만 공백을 지웁니다. `{ } ; , : > (` 뒤와 `{ } ; , > )` 앞입니다.
`}` 앞의 `;`도 지웁니다. 그 밖의 연속된 공백은 공백 하나가 되므로 `div :hover`,
`and (min-width: ...)`, 그리고 calc()가 `+`와 `-` 주위에 필요로 하는 공백이 살아남습니다. 문자열,
`.md\:flex` 같은 이스케이프, 따옴표 없는 `url(...)` 인자는 그대로 복사됩니다. 주석을 지워도 두
토큰이 합쳐지지 않습니다. 자기 출력에 최소화기를 다시 실행해도 바뀌는 것이 없습니다.

`cwist_css_bundle()`은 부분들을 순서대로 줄바꿈으로 이어 붙이고, 결과를 최소화할 수 있습니다.
`@import`는 해석하지 않습니다.

### `cwist_app_asset_add` / `cwist_app_asset_add_file` / `cwist_app_asset_url`
```c
cwist_error_t cwist_app_asset_prefix(cwist_app *app, const char *url_prefix);
cwist_error_t cwist_app_asset_add(cwist_app *app, const char *name, const void *data,
                                  size_t len, const char *content_type);
cwist_error_t cwist_app_asset_add_file(cwist_app *app, const char *name, const char *path,
                                       const char *content_type);
const char *cwist_app_asset_url(cwist_app *app, const char *name);
```
`css/app.css`로 등록한 자산은 `/assets/css/app.<16자리 16진수>.css`에서 제공됩니다. 숫자는 바이트의
SHA-256 앞부분이며, `ETag`, `Cache-Control: public, max-age=31536000, immutable`, 그리고 일치하는
`If-None-Match`에 대한 `304 Not Modified`가 붙습니다. 같은 바이트는 모든 프로세스에서, 재시작을
넘어서도 항상 같은 URL을 줍니다. 새 바이트는 새 URL을 주고, 이전 URL은 앱이 파괴될 때까지 이전
바이트를 계속 제공하므로, 어딘가에 아직 캐시된 페이지도 스타일을 계속 불러옵니다. 해시를 알 수 없는
도구를 위해 논리 이름(`/assets/css/app.css`)도 `Cache-Control: no-cache`로 제공됩니다.

자산은 라우트 미스 시 정적 디렉터리보다 먼저, 앱의 미들웨어 체인을 거쳐 응답됩니다. 파일 시스템이
필요 없으므로 WASM 빌드에서도 동작합니다. 콘텐츠 타입은 주어지지 않으면 확장자에서 정합니다.
이름은 문자, 숫자, `.`, `-`, `_`, `~`와 `/`로 나뉜 세그먼트로 제한되며, `.`과 `..` 세그먼트는
거부됩니다. 자산은 `cwist_app_listen()` 전에 등록하세요. prefork 워커는 fork된 시점에 있던 것만
봅니다. `cwist_multiport_get_app()`으로 분리한 포트는 그 시점에 등록된 자산의 복사본을 받습니다. 모든
함수는 `err_i16` 채널로 0 또는 -1을 반환합니다.

### 모두 합치기

```c
/* At startup, before cwist_app_listen(). */
cwist_sstring *scoped = cwist_css_scope_generate_stylesheet(&card_css);
const char *parts[] = {base_css, scoped->data};
cwist_sstring *bundle = cwist_css_bundle(parts, 2, true);
cwist_app_asset_add(app, "app.css", bundle->data, bundle->size, NULL);
cwist_sstring_destroy(bundle);
cwist_sstring_destroy(scoped);

/* In the layout component's render function. */
cwist_html_element_t *link = cwist_html_element_create("link");
cwist_html_element_add_attr(link, "rel", "stylesheet");
cwist_html_element_add_attr(link, "href", cwist_app_asset_url(app, "app.css"));
```

## WASM에서 같은 뷰 렌더링하기

위의 컴포넌트, 범위 CSS, 페이지/프래그먼트, 자산 코드는 WASM 빌드(`libcwist_wasm.a`, 그리고 같은
소스 목록으로 빌드하는 WASI 0.2 아카이브)에 들어 있습니다. WASM으로 컴파일한 앱은 서버와 같은
함수로 렌더링하므로, 렌더러가 두 개가 아니라 하나입니다.

`cwist_app_dispatch_memory()`는 원시 요청 바이트를 받아 원시 응답 바이트를 반환하며,
`CWIST_WASM_DEFINE_ENTRY`가 JavaScript에 노출하는 것이 바로 이것입니다. 그래서 뷰 코드 한 벌을 세
곳에서 쓸 수 있습니다.

- 서버에서, 첫 페이지 로드를 위해;
- Service Worker에서, 이후 페이지가 보내는 프래그먼트 요청에 왕복 없이 응답하기 위해
  (`example/wasm-service-worker`의 `GET /items/list` 라우트 참고);
- HTTP 형태의 요청을 전달하는 그 밖의 모든 WASM 호스트에서.

`cwist_app_asset_add()`로 등록한 자산은 파일 시스템을 건드리지 않으므로 WASM 앱도 제공합니다.
`cwist_app_asset_add_file()`은 호스트가 제공하는 파일 시스템이 필요합니다.

이 동등성은 가정하지 않고 확인합니다. `tests/html_views_shared.h`는 레이아웃과 카드 컴포넌트,
해시 자산으로 묶은 범위 스타일시트, 페이지/프래그먼트/대역 외/리다이렉트 라우트, 그리고 각 요청이
만들어야 하는 정확한 응답 바이트를 정의합니다. `test_html_parity`는 네이티브 빌드에서 이 검사를
실행하고, `make wasm-smoke`는 SHA-256에서 나온 자산 URL과 FNV-1a 범위 접미사를 포함해 같은 검사를
Emscripten과 node에서 실행합니다.

## 예제

```c
#include <cwist/core/html/component.h>
#include <cwist/core/html/css_composer.h>

typedef struct {
    const char *title;
    cwist_css_scope *css;
} card_props;

static cwist_html_element_t *card_render(const void *props, cwist_html_element_t **children,
                                         size_t child_count) {
    const card_props *p = props;
    cwist_html_element_t *root = cwist_html_element_create("div");
    if (!root) {
        /* The render function owns the children, including on failure. */
        for (size_t i = 0; i < child_count; i++) cwist_html_element_destroy(children[i]);
        return NULL;
    }
    cwist_html_element_add_class(root, cwist_css_scope_class(p->css, "card"));

    cwist_html_element_t *h2 = cwist_html_element_create("h2");
    cwist_html_element_set_text(h2, p->title);
    cwist_html_element_add_child(root, h2);
    for (size_t i = 0; i < child_count; i++) cwist_html_element_add_child(root, children[i]);
    return root;
}

/* ... */
cwist_html_component_t *card = cwist_html_component_create("card", card_render);
cwist_css_scope css;
cwist_css_scope_init(&css, cwist_html_component_name(card));
cwist_css_scope_add_rule(&css, "card", "padding: 1rem; border-radius: 8px;");

cwist_html_element_t *body = cwist_html_element_create("p");
cwist_html_element_set_text(body, "Hello");
card_props props = {"Welcome", &css};
cwist_html_element_t *el = cwist_html_component_instantiate(card, &props, &body, 1);

cwist_sstring *html = cwist_html_render(el);             /* <div class="card-8827595f">... */
cwist_sstring *style = cwist_css_scope_generate_stylesheet(&css);
/* .card-8827595f { padding: 1rem; border-radius: 8px; } */

cwist_sstring_destroy(style);
cwist_sstring_destroy(html);
cwist_html_element_destroy(el);
cwist_css_scope_destroy(&css);
cwist_html_component_destroy(card);
```
