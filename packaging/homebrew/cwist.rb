# Homebrew formula for CWIST. Maintained here and mirrored into the tap
# repository c4punks/homebrew-cwist as Formula/cwist.rb.
#
# Before publishing a release:
#   1. make dist
#   2. upload dist/cwist-<version>.tar.gz to the GitHub release assets
#   3. update `url` and `sha256` below
class Cwist < Formula
  desc "C17 web framework and application server (HTTP/1.1, HTTP/2, HTTP/3, WebSocket, PQC TLS)"
  homepage "https://github.com/c4punks/CWIST"
  url "https://github.com/c4punks/CWIST/releases/download/v3.6/cwist-3.6.tar.gz"
  sha256 "d003d9f7d8e6af8cbca4e80628c4799c96da6f4598fc0569a347ffb1c24c1414"
  # The framework itself is MIT; the vendored components it statically links
  # are Apache-2.0 (BoringSSL, cnats), MIT (lsquic, cJSON,
  # multipart-parser-c), BSD-3-Clause (libttak, uriparser, lsquic's Chromium
  # portions), and SQLite (public domain, SPDX "blessing").
  license all_of: ["MIT", "Apache-2.0", "BSD-3-Clause", :public_domain]

  depends_on "cmake" => :build
  depends_on "pkg-config" => :build
  depends_on "brotli"
  depends_on "zstd"

  uses_from_macos "zlib"
  uses_from_macos "curl"

  def install
    system "make", "-j#{ENV.make_jobs}", "VERSION=3.6"
    system "make", "install", "PREFIX=#{prefix}", "VERSION=3.6"
  end

  test do
    (testpath/"test.c").write <<~EOS
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
    EOS
    flags = shell_output("pkg-config --cflags --libs cwist").split
    system ENV.cc, "-o", "test", "test.c", *flags
    system "./test"
  end
end
