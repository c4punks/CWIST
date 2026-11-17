# Nix derivation for CWIST. Maintained here and intended for packaging into
# nixpkgs as pkgs/by-name/cw/cwist/package.nix.
{ lib
, stdenv
, fetchFromGitHub
, cmake
, pkg-config
, brotli
, zstd
, zlib
, curl
, nghttp2
}:

stdenv.mkDerivation rec {
  pname = "cwist";
  version = "4.3.1";

  src = fetchFromGitHub {
    owner = "c4punks";
    repo = "CWIST";
    rev = "v${version}";
    hash = "sha256-0000000000000000000000000000000000000000000=";
    fetchSubmodules = true;
  };

  nativeBuildInputs = [
    cmake
    pkg-config
  ];

  dontUseCmakeConfigure = true;

  buildInputs = [
    brotli
    zstd
    zlib
    curl
    nghttp2
  ];

  makeFlags = [
    "PREFIX=${placeholder \"out\"}"
    "VERSION=${version}"
    "CWIST_SQLITE_FTS5=1"
  ];

  passthru.tests = {
    smoke = stdenv.mkDerivation {
      name = "cwist-test-smoke";
      nativeBuildInputs = [ pkg-config ];
      buildInputs = [ (placeholder "out") ];
      buildCommand = ''
        cat << 'EOF' > test.c
        #include <cwist/app.h>
        static void hello(cwist_http_request *req, cwist_http_response *res) {
            (void)req;
            cwist_sstring_assign(res->body, "ok");
        }
        int main(void) {
            cwist_app *app = cwist_app_create();
            cwist_app_get(app, "/", hello);
            cwist_app_destroy(app);
            return 0;
        }
        EOF
        $CC -o test test.c $(pkg-config --cflags --libs cwist)
        ./test
        touch $out
      '';
    };
  };

  meta = with lib; {
    description = "C17 web framework and application server (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, PQC TLS)";
    homepage = "https://github.com/c4punks/CWIST";
    license = with licenses; [ mit asl20 bsd3 unlicense ];
    platforms = platforms.unix;
    mainProgram = "cwist";
  };
}
