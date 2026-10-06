#!/usr/bin/env python3
"""Generate sitemap.xml and robots.txt for GitHub Pages deployed Doxygen site."""
import os
import sys
from pathlib import Path
from datetime import datetime, timezone

BASE_URL = "https://c4punks.github.io/CWIST"

def generate_sitemap(html_dir: Path):
    if not html_dir.exists():
        print(f"Error: {html_dir} does not exist", file=sys.stderr)
        return

    now = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    html_files = []

    for root, _, files in os.walk(html_dir):
        for file in files:
            if file.endswith(".html"):
                full_path = Path(root) / file
                rel_path = full_path.relative_to(html_dir).as_posix()
                html_files.append(rel_path)

    html_files.sort()

    sitemap_lines = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        '<urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">',
    ]

    for rel in html_files:
        url = f"{BASE_URL}/{rel}"
        # Prioritize main pages
        if rel in ("index.html", "pages.html"):
            priority = "1.0"
            changefreq = "daily"
        elif rel.startswith("md_"):
            priority = "0.8"
            changefreq = "weekly"
        else:
            priority = "0.6"
            changefreq = "monthly"

        sitemap_lines.append(f"  <url>")
        sitemap_lines.append(f"    <loc>{url}</loc>")
        sitemap_lines.append(f"    <lastmod>{now}</lastmod>")
        sitemap_lines.append(f"    <changefreq>{changefreq}</changefreq>")
        sitemap_lines.append(f"    <priority>{priority}</priority>")
        sitemap_lines.append(f"  </url>")

    sitemap_lines.append('</urlset>')

    sitemap_file = html_dir / "sitemap.xml"
    sitemap_file.write_text("\n".join(sitemap_lines) + "\n", encoding="utf-8")
    print(f"Generated {sitemap_file} with {len(html_files)} entries.")

    robots_content = f"""User-agent: *
Allow: /

Sitemap: {BASE_URL}/sitemap.xml
"""
    robots_file = html_dir / "robots.txt"
    robots_file.write_text(robots_content, encoding="utf-8")
    print(f"Generated {robots_file}.")

if __name__ == "__main__":
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path("docs/doxygen/html")
    generate_sitemap(target)
